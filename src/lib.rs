#![no_std]
#![feature(naked_functions)]

#[repr(C)]
struct Stat {
    dev: i32, // file system's disk device
    ino: u32, // inode number
    r#type: i16, // type of file
    nlink: i16, // number of links to file
    size: u64, // size of file in bytes
}

// int fork(void);
#[naked]
#[no_mangle]
unsafe extern "C" fn fork() -> i32 {
    core::arch::asm!("li a7, 1", "call exit", "ret", options(noreturn))
}

// int exit(int) __attribute__((noreturn));
#[naked]
#[no_mangle]
unsafe extern "C" fn exit(status: i32) -> i32 {
    core::arch::asm!("li a7, 2", "ecall", "ret", options(noreturn))
}

// int wait(int *);
#[naked]
#[no_mangle]
unsafe extern "C" fn wait(wstatus: *const i32) -> i32 {
    core::arch::asm!("li a7, 3", "ecall", "ret", options(noreturn))
}

// int pipe(int *);
#[naked]
#[no_mangle]
unsafe extern "C" fn pipe(pipefd: *const i32) -> i32 {
    core::arch::asm!("li a7, 4", "ecall", "ret", options(noreturn))
}

// int read(int, void *, int);
#[naked]
#[no_mangle]
unsafe extern "C" fn read(fd: i32, buf: *const u8, count: usize) -> i32 {
    core::arch::asm!("li a7, 5", "ecall", "ret", options(noreturn))
}

// int kill(int);
#[naked]
#[no_mangle]
unsafe extern "C" fn kill(pid: i32) -> i32 {
    core::arch::asm!("li a7, 6", "ecall", "ret", options(noreturn))
}

// int exec(char *, char **);
#[naked]
#[no_mangle]
unsafe extern "C" fn exec(path: *const u8, argv: *const *const u8) -> i32 {
    core::arch::asm!("li a7, 7", "ecall", "ret", options(noreturn))
}

// int fstat(int fd, const struct stat *);
#[naked]
#[no_mangle]
unsafe extern "C" fn fstat(fd: i32, statbuf: *const Stat) -> i32 {
    core::arch::asm!("li a7, 8", "ecall", "ret", options(noreturn))
}

// int chdir(const char *);
#[naked]
#[no_mangle]
unsafe extern "C" fn chdir(path: *const u8) -> i32 {
    core::arch::asm!("li a7, 9", "ecall", "ret", options(noreturn))
}

// int dup(int);
#[naked]
#[no_mangle]
unsafe extern "C" fn dup(oldfd: i32) -> i32 {
    core::arch::asm!("li a7, 10", "ecall", "ret", options(noreturn))
}

// int getpid(void);
#[naked]
#[no_mangle]
unsafe extern "C" fn getpid() -> i32 {
    core::arch::asm!("li a7, 11", "ecall", "ret", options(noreturn))
}

// char *sbrk(int);
#[naked]
#[no_mangle]
unsafe extern "C" fn sbrk(incr: i32) -> *const u8 {
    core::arch::asm!("li a7, 12", "ecall", "ret", options(noreturn))
}

// int sleep(int);
#[naked]
#[no_mangle]
unsafe extern "C" fn sleep(sec: i32) -> i32 {
    core::arch::asm!("li a7, 13", "ecall", "ret", options(noreturn))
}

// int uptime(void);
#[naked]
#[no_mangle]
unsafe extern "C" fn uptime() -> i32 {
    core::arch::asm!("li a7, 14", "ecall", "ret", options(noreturn))
}

// int open(const char *, int);
#[naked]
#[no_mangle]
unsafe extern "C" fn open(path: *const u8, flags: i32) -> i32 {
    core::arch::asm!("li a7, 15", "ecall", "ret", options(noreturn))
}

// int write(int, const void *, int);
#[naked]
#[no_mangle]
unsafe extern "C" fn write() -> ! {
    core::arch::asm!("li a7, 16", "ecall", "ret", options(noreturn))
}

// int mknod(const char *, short, short);
#[naked]
#[no_mangle]
unsafe extern "C" fn mknod() -> ! {
    core::arch::asm!("li a7, 17", "ecall", "ret", options(noreturn))
}

// int unlink(const char *);
#[naked]
#[no_mangle]
unsafe extern "C" fn unlink() -> ! {
    core::arch::asm!("li a7, 18", "ecall", "ret", options(noreturn))
}

// int link(const char *, const char *);
#[naked]
#[no_mangle]
unsafe extern "C" fn link() -> ! {
    core::arch::asm!("li a7, 19", "ecall", "ret", options(noreturn))
}

// int mkdir(const char *);
#[naked]
#[no_mangle]
unsafe extern "C" fn mkdir() -> ! {
    core::arch::asm!("li a7, 20", "ecall", "ret", options(noreturn))
}

// int close(int);
#[naked]
#[no_mangle]
unsafe extern "C" fn close() -> ! {
    core::arch::asm!("li a7, 21", "ecall", "ret", options(noreturn))
}

pub fn add(left: u64, right: u64) -> u64 {
    left + right
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn it_works() {
        let result = add(2, 2);
        assert_eq!(result, 4);
    }
}
