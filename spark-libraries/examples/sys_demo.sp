$import "sys"

fn main()
    print("SPARK SYS MODULE - EXPERIMENTAL LINUX SYSCALLS\n")
    print("Process ID: " + str(sys_getpid()) + "\n")
    print("Parent ID:  " + str(sys_getppid()) + "\n")
    print("User ID:    " + str(sys_getuid()) + "\n")
    print("Group ID:   " + str(sys_getgid()) + "\n\n")

    let fd = sys_open("spark_syscall_test.txt", 577, 420)
    if fd < 0
        print("open failed; errno=" + str(sys_errno()) + "\n")
        return

    let written = sys_write(fd, "Hello from Spark syscalls!\n")
    print("write returned: " + str(written) + "\n")
    sys_close(fd)

    let rd = sys_open("spark_syscall_test.txt", 0, 0)
    if rd < 0
        print("reopen failed; errno=" + str(sys_errno()) + "\n")
        return

    let contents = sys_read(rd, 256)
    print("read returned: " + str(sys_read_count()) + " bytes\n")
    print("File says: " + contents)
    sys_close(rd)

    let removed = sys_unlink("spark_syscall_test.txt")
    print("unlink returned: " + str(removed) + "\n")

    let slept = sys_sleep_ms(25)
    print("nanosleep returned: " + str(slept) + "\n")
    print("Done. Check sys_errno() after any -1 result.\n")
