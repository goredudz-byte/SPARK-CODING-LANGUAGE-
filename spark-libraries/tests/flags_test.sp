$import "sys"
$import "../modules/sys_flags.sp"
fn main()
    let fd = sys_open("flags_test.txt", SP_O_WRONLY + SP_O_CREAT + SP_O_TRUNC, SP_S_IRUSR + SP_S_IWUSR + SP_S_IRGRP + SP_S_IROTH)
    if fd < 0
        print("open failed: " + str(sys_errno()) + "\n")
        return
    sys_write(fd, "flags module works\n")
    sys_close(fd)
    print("flags module works\n")
    sys_unlink("flags_test.txt")
