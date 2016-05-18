1. ifndef、if defined 
    // if defined 可以组成更加复杂的编译条件
    #ifndef __need_IOV_MAX // 如果没有定义 __need_IOV_MAX
    #define __need_IOV_MAX // 定义__need_IOV_MAX
    #endif                 // 结束if