1. ifndef、if defined 
    // if defined 可以组成更加复杂的编译条件
    #ifndef __need_IOV_MAX // 如果没有定义 __need_IOV_MAX
    #define __need_IOV_MAX // 定义__need_IOV_MAX
    #endif                 // 结束if
    
2. time
    time(0) 当前时间距离格林威治时间的秒数