#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <linux/kvm.h>

#define CODE_START 0x0
#define GUEST_PATH "../xv6/xv6.img"
#define START_ADDRESS 0x7c00
#define GUEST_MEMORY_SIZE 0x80000000
#define ALIGNMENT_SIZE 0x1000
#define DEFAULT_FLAGS 0x0000000000000002ULL
#define GUEST_BINARY_SIZE (4096 * 128)
#define IMGE_SIZE 5120000

typedef uint8_t u8;
typedef uint8_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct kvm_userspace_memory_region kvm_mem;

struct vm {
    int vm_fd;
    int fd;
    void *mem;
};

struct vcpu {
    int fd;
    struct kvm_run *kvm_run;
    struct kvm_sregs sregs;
    struct kvm_regs regs;
};

struct kvm_lapic;

struct blk {
    u8 *data;
    u16 data_reg;
    u8 sec_count_reg;
    u8 lba_low_reg;
    u8 lba_middle_reg;
    u8 lba_high_reg;
    u8 drive_head_reg;
    u8 status_command_reg;
    u32 index;
};

struct io {
    __u8 direction;
    __u8 size;
    __u16 port;
    __u32 count;
    __u64 data_offset; 
};

int outfd = 0;

void error(char *message) {
    perror(message);
    exit(1);
}

void init_vcpu(struct vm *vm, struct vcpu *vcpu) {
    int mmap_size;

    vcpu->fd = ioctl(vm->fd, KVM_CREATE_VCPU, 0);
    
    if (vcpu->fd < 0) {
        error("KVM_CREATE_VCPU");
    }

    mmap_size = ioctl(vm->vm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);

    if (mmap_size <= 0) {
        error("KVM_GET_VCPU_MMAP_SIZE");
    }

    vcpu->kvm_run = mmap(NULL, mmap_size, 
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED, vcpu->fd, 0);

    if (vcpu->kvm_run == MAP_FAILED) {
        perror("kvm_run: failed\n");
        exit(1);
    }
}

void set_sregs(struct kvm_sregs *sregs) {
    sregs->cs.selector = 0;
	sregs->cs.base = 0;
}

void set_regs(struct vcpu *vcpu) {
    if (ioctl(vcpu->fd, KVM_GET_SREGS, &(vcpu->sregs)) < 0) {
        error("KVM_GET_SREGS");
    }

    set_sregs(&vcpu->sregs);

    if (ioctl(vcpu->fd, KVM_SET_SREGS, &(vcpu->sregs)) < 0) {
        error("KVM_SET_SREGS");
    }

    vcpu->regs.rflags = DEFAULT_FLAGS;
    vcpu->regs.rip = START_ADDRESS;

    if (ioctl(vcpu->fd, KVM_SET_REGS, &(vcpu->regs)) < 0) {
        error("KVM_SET_REGS");
    }
}

void create_irqchip(int fd) {
    if (ioctl(fd, KVM_CREATE_IRQCHIP, 0) < 0) {
        error("KVM_CREATE_IRQCHIP");
    }
}

void load_guest_binary(void *dst) {
    int fd = open("../xv6/bootblock", O_RDONLY);
    if (fd < 0) 
        error("open fail");
    
    void *tmp = dst;
    for(;;) {
        int size = read(fd, tmp + START_ADDRESS, 
                        GUEST_BINARY_SIZE);
        if (size <= 0) 
            break;
        tmp += size;
    }
}

void memalign(void **dst, size_t size, size_t align) {
    if (posix_memalign(dst, size, align) < 0) {
        printf("memalign: faile\n");
        exit(1);
    }
}

void set_vm_mem(struct vm *vm, 
                            kvm_mem *memreg,
                            u64 phys_start,
                            size_t size) {

    memalign(&(vm->mem), size, ALIGNMENT_SIZE);

    memreg->slot = 0;
	memreg->flags = 0;
	memreg->guest_phys_addr = phys_start;
	memreg->memory_size = (u64)size;
	memreg->userspace_addr = (unsigned long)vm->mem;
    
    if (ioctl(vm->fd, KVM_SET_USER_MEMORY_REGION, memreg) < 0) {
		error("KVM_SET_USER_MEMORY_REGION");
	}
}

void print_regs(struct vcpu *vcpu) {
    ioctl(vcpu->fd, KVM_GET_REGS, &(vcpu->regs));
    printf("rip: 0x%llx\n", vcpu->regs.rip);
}

void create_blk(struct blk *blk) {
    blk->data = malloc(IMGE_SIZE);
    blk->data_reg = 0;
    blk->drive_head_reg = 0;
    blk->lba_high_reg = 0;
    blk->lba_middle_reg = 0;
    blk->lba_low_reg = 0;
    blk->sec_count_reg = 0;
    blk->status_command_reg = 0x40;

    int img_fd = open("../xv6/xv6.img", O_RDONLY);
    if (img_fd < 0) {
        perror("fail open xv6.img");
        exit(1);
    }

    void *tmp = (void*)blk->data;
    for(;;) {
        int size = read(img_fd, tmp, IMGE_SIZE);
        if (size <= 0) 
            break;

        tmp += size;
    }
}

void handle_io_out(struct blk *blk, int port, char value, u16 val) {
    switch (port)
    {
    case 0x1F0:
        blk->data_reg = val;
        break;
    case 0x1F2:
        blk->sec_count_reg = value;
        break;
    case 0x1F3:
        blk->lba_low_reg = value;
        break;            
    case 0x1F4:
        blk->lba_middle_reg = value;
        break;
    case 0x1F5:
        blk->lba_high_reg = value;
    case 0x1F6:
        blk->drive_head_reg = value;
        break;
    case 0x1F7:
        if (value == 0x20) {
            u32 i = 0 | blk->lba_low_reg | (blk->lba_middle_reg << 8) | (blk->lba_high_reg << 16) |
                          ((blk->drive_head_reg & 0x0F) << 24);
            //printf("i: %d\n", i);
            blk->index = i * 512;
            break;
        }

        printf("value: %d\n", value);
        break;
    case 0x8a00:
        printf("error 0x8a00\n");
        exit(1);
        break;
    default:
        break;
    }
}

void init_kvm(struct vm *vm) {
    vm->vm_fd = open("/dev/kvm", O_RDWR);
    if (vm->vm_fd < 0) { 
        error("open /dev/kvm");
    }
}

void create_vm(struct vm *vm) {
    vm->fd = ioctl(vm->vm_fd, KVM_CREATE_VM, 0);
    if (vm->fd < 0) {
        error("KVM_CREATE_VM");
    }
}

void create_pit(int fd) {
    if (ioctl(fd, KVM_CREATE_PIT) < 0) {
        error("KVM_CREATE_PIT");
    }
}

void set_tss(int fd) {
    if (ioctl(fd, KVM_SET_TSS_ADDR, 0xfffbd000) < 0) {
        error("KVM_SET_TSS_ADDR");
    }
}

void create_output_file() {
    outfd = open("out.txt", O_RDWR | O_CREAT);
}

void emulate_diskr(struct blk *blk) {
    u32 i = 0 | blk->lba_low_reg | (blk->lba_middle_reg << 8) | (blk->lba_high_reg << 16) |
            ((blk->drive_head_reg & 0x0F) << 24);
    blk->index = i * 512;
}

void emulate_disk_port(struct vcpu *vcpu, 
                        struct blk *blk, struct io io) {
    u8 val1;
    u16 val2;

    if (io.size != 4)
        return;
    
    if (io.port == 0x1F0) {
        val2 = *(u16*)((u16*)vcpu->kvm_run + io.data_offset);
        blk->data_reg = val2;
        return;
    }

    val1 = *(u8*)((u8*)vcpu->kvm_run + io.data_offset);
    
    switch (io.port) {
    case 0x1F2:
        blk->sec_count_reg = val1;
        break;
    case 0x1F3:
        blk->lba_low_reg = val1;
        break;            
    case 0x1F4:
        blk->lba_middle_reg = val1;
        break;
    case 0x1F5:
        blk->lba_high_reg = val1;
    case 0x1F6:
        blk->drive_head_reg = val1;
        break;
    case 0x1F7:
        if (val1 == 0x20) {
            emulate_diskr(blk);
            break;
        }

        break;
    default:
        break;
    }
}

void emulate_uart_port(struct vcpu *vcpu, struct io io) {
    if (io.port != 0x3f8) return;

    for (int i = 0; i < io.count; i++) {
        char *v = (char*)((unsigned char*)vcpu->kvm_run + io.data_offset);
        write(outfd, v, 1);
        io.data_offset += io.size;
    }
}

void emulate_io_out(struct vcpu *vcpu, struct blk *blk, struct io io) {
    switch (io.port)
    {
    case 0x1F0 ... 0x1F7:
        emulate_disk_port(vcpu, blk, io);
        break;
    case 0x3F8:
        emulate_uart_port(vcpu, io);
        break;
    default:
        break;
    }
}

void emulate_io(struct vcpu *vcpu, struct blk *blk) {
    struct io io = {
        .direction = vcpu->kvm_run->io.direction,
        .size = vcpu->kvm_run->io.size,
        .port = vcpu->kvm_run->io.port,
        .count = vcpu->kvm_run->io.count,
        .data_offset = vcpu->kvm_run->io.data_offset
    };

    switch (io.direction) {
    case KVM_EXIT_IO_OUT:
        printf("out: %d\n", io.port);
        print_regs(vcpu);
        emulate_io_out(vcpu, blk, io);
        break;
    case KVM_EXIT_IO_IN:
        break;
    default:
        break;
    }
}

int main(int argc, char **argv) {
    struct vm *vm = malloc(sizeof(struct vm));
    struct vcpu *vcpu = malloc(sizeof(struct vcpu));
    struct blk *blk = malloc(sizeof(struct blk));
    struct kvm_lapic_state *lapic = malloc(sizeof(struct kvm_lapic_state));
    kvm_mem *memreg = malloc(sizeof(kvm_mem));

    init_kvm(vm);
    create_vm(vm);
    create_irqchip(vm->fd);
    create_pit(vm->fd);
    create_blk(blk);
    //set_tss(vm->fd);
    set_vm_mem(vm, memreg, 0, GUEST_MEMORY_SIZE);    
    load_guest_binary(vm->mem);
    init_vcpu(vm, vcpu);
    set_regs(vcpu);

    create_output_file();

    for (;;) {
        if (ioctl(vcpu->fd, KVM_RUN, 0) < 0) {
            error("KVM_RUN");
        }

        struct kvm_run *run = vcpu->kvm_run;

        // switch (run->exit_reason) {
        // case KVM_EXIT_IO:
        //     emulate_io(run, vcpu);
        //     break;
        // case KVM_EXIT_MMIO:
        //     break;
        // default:
        //     break;
        // }

        int port = vcpu->kvm_run->io.port;
        u32 d = 0;

        if (vcpu->kvm_run->exit_reason == KVM_EXIT_IO) {

            if (vcpu->kvm_run->io.direction == KVM_EXIT_IO_OUT) {
                //emulate_io(vcpu, blk);
                for (int i = 0; i < vcpu->kvm_run->io.count; i++) {

                    char value = *(unsigned char *)((unsigned char *)vcpu->kvm_run + vcpu->kvm_run->io.data_offset);
                    u16 val = *(u16 *)((u16 *)vcpu->kvm_run + vcpu->kvm_run->io.data_offset);
                    ioctl(vcpu->fd, KVM_GET_REGS, &(vcpu->regs));

                    if (port == 0x3f8) {
                        char value[100];
                        for (int i_out = 0; i_out < vcpu->kvm_run->io.count; i_out++) {
                            char *v = (char*)((unsigned char*)vcpu->kvm_run + vcpu->kvm_run->io.data_offset);
                            write(outfd, v, 1);
                            vcpu->kvm_run->io.data_offset += vcpu->kvm_run->io.size;
                        }
                        break;
                    }
                    printf("out: %d\n", vcpu->kvm_run->io.port);
                    print_regs(vcpu);
                    handle_io_out(blk, port, value, val);
		       }
            } else {
                //printf("in: %u\n", port);
                //printf("offset: 0x%llx\n", vcpu->kvm_run->io.data_offset);
                //print_regs(vcpu);
                switch (port)
                {
                case 0x1F7:
                    *(unsigned char *)((unsigned char *)vcpu->kvm_run + vcpu->kvm_run->io.data_offset) = blk->status_command_reg; 
                    break;
                case 0x1F0:
                    for (int i = 0; i < vcpu->kvm_run->io.count; ++i) {
                        for (int j = 0; j < 4; j++) {
                            //printf("blk index: %d\n", blk->index);
                            d |= blk->data[blk->index] << (8 * j);
                            blk->index += 1;
                        }
                        // 7f 0001111111
                        // 45 000000000001000101
                        //break;
                        // 46 4c 45 7f = 01000110010011000100010101111111 

                        *(u32 *)((unsigned char *)vcpu->kvm_run + vcpu->kvm_run->io.data_offset) = d;
                        vcpu->kvm_run->io.data_offset += vcpu->kvm_run->io.size;
                        d = 0;
                    }
                    
                    break;
                case 0x3fc:
                    break;
                default:
                    break;
                }
            }
        } else if (vcpu->kvm_run->exit_reason == KVM_EXIT_HLT) {
            print_regs(vcpu);
            printf("HLT\n");
            exit(1);
        } else if (vcpu->kvm_run->exit_reason == KVM_EXIT_MMIO) {
            print_regs(vcpu);
            printf("mmio phys: 0x%llx\n", vcpu->kvm_run->mmio.phys_addr);
            printf("data: 0x%lx\n", (u64)vcpu->kvm_run->mmio.data);
            printf("len: %d\n", vcpu->kvm_run->mmio.len);

            if (vcpu->kvm_run->mmio.is_write) {
                printf("is write\n");
            }

            if (ioctl(vcpu->fd, KVM_GET_LAPIC, lapic) < 0) {
                error("KVM_GET_LAPIC");
            }

            u32 data = 0;
            for (int i = 0; i < 4; i++) {
                data |= vcpu->kvm_run->mmio.data[i] << i*8;
            }

            printf("data: 0x%x\n", data);

            int index = vcpu->kvm_run->mmio.phys_addr - 0xffe00000;
            printf("index: %d\n", index);

            lapic->regs[index/4] = data;
            if (ioctl(vcpu->fd, KVM_SET_LAPIC, lapic) < 0) {
                error("KVM_SET_LAPIC");
            }
        } else {
            print_regs(vcpu);
            printf("exit reason: %d\n", vcpu->kvm_run->exit_reason);
            exit(1);
        }
    }

    return 1;   
}