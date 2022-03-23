# libhv接口手册
# base基础设施
base模块包含了一些c/c++基础设施，如常用宏定义、日期时间、字符串、文件、目录、进程、线程、套接字、日志、缓存等；

## hatomic.h：原子操作
原子标记hatomic_flag_t
```
hatomic_flag_t flag = HATOMIC_FLAG_INIT;
hatomic_flag_test_and_set(&flag); // 原子测试并置位
```
原子数hatomic_t
```
hatomic_t cnt = HATOMIC_VAR_INIT(0);
hatomic_inc(&cnt); // 原子自增
hatomic_dec(&cnt); // 原子自减
hatomic_add(&cnt, 3); // 原子加
hatomic_sub(&cnt, 3); // 原子减
```
测试代码见：https://github.com/ithewei/libhv/blob/master/unittest/hatomic_test.c

## hbase.h：基本接口
### 安全alloc/free
safe_malloc
safe_realloc
safe_calloc
safe_zalloc
safe_free
HV_ALLOC
HV_ALLOC_SIZEOF
HV_FREE
HV_MEMCHECK：程序退出时打印alloc/free计数以判断程序是否有内存泄露
### 字符串操作
strupper：字符串转大写
strlower：字符串转小写
strreverse：字符串翻转
strstartswith：判断字符串是否以xxx开头
strendswith：判断字符串是否以xxx结尾
strcontains：判断字符串是否包含xxx
safe_strncpy：安全strncpy
safe_strncat：安全strncat
strrchr_dot：查找最后一个点（通常用于提取文件后缀）
strrchr_dir：查找最后的路径（通常用于分离目录和文件）
hv_basename：获取文件名（利用了上面的strrchr_dir）
hv_suffixname：获取文件后缀（利用了上面的strrchr_dot）
getboolean：1 y on yes true enable返回true（通常用于配置文件）
### 获取运行时路径
get_executable_path：获取可执行文件绝对路径，例如/usr/local/bin/httpd
get_executable_dir：获取可执行文件所在目录，例如/usr/local/bin
get_executable_file：获取可执行文件名，例如httpd
get_run_dir：获取运行目录，例如/home/www/html
### 其它
hv_mkdir_p：递归创建目录
hv_rmdir_p：递归删除目录
hv_exists: 判断路径是否存在
hv_isdir: 判断是否是目录
hv_isfile: 判断是否是文件
hv_islink: 判断是否是链接
hv_filesize: 获取文件大小

## hbuf.h：缓存
### c普通buffer：hbuf_t
```c
typedef struct hbuf_s {
	char* base; // 指针起始地址
	size_t len; // buffer长度
} hbuf_t;
```
### 带偏移量buffer：offset_buf_t
```c
typedef struct offset_buf_s {
	char* base; // 指针起始地址
	size_t len; // buffer长度
	size_t offset; // 偏移量
} offset_buf_t;
```
通常用于消费了一部分数据，需要记录下当前偏移。如用作socket写缓存，当一次写数据包过大时，一次无法发送全部数据，可以记录下当前发送的偏移量，当socket可写时再继续发送剩余的。

### c++普通buffer：HBuf
```c++
class HBuf : public hbuf_t { // 继承自c普通buffer
public:
	HBuf();
	HBuf(void* data, size_t len);
	HBuf(size_t cap) { resize(cap); }
	// 析构时会自动释放内存
	virtual ~HBuf() { cleanup(); } 

	void*  data() { return base; }
	size_t size() { return len; }
	bool isNull() { return base == NULL || len == 0; }

	void cleanup(); // free内存
	void resize(size_t cap); // realloc
	// 深拷贝（resize后memcpy）
	void copy(void* data, size_t len);
	void copy(hbuf_t* buf);
};
```
### 可变长buffer：HVLBuf
```c++
class HVLBuf : public HBuf { // VL: Variable-Length可变长
public:
	HVLBuf();
	HVLBuf(void* data, size_t len);
	HVLBuf(size_t cap);
	virtual ~HVLBuf() {}
	
	char* data() { return base + _offset; } // 返回当前偏移地址
	size_t size() { return _size; } // 返回实际已存储数据量
	void push_front(void* ptr, size_t len);
	void push_back(void* ptr, size_t len);
	void pop_front(void* ptr, size_t len);
	void pop_back(void* ptr, size_t len);
	void clear();
	
	void prepend(void* ptr, size_t len); // alias push_front
	void append(void* ptr, size_t len); // alias push_back
	void insert(void* ptr, size_t len); // alias push_back
	void remove(size_t len); // alias pop_front
	
private:
	size_t _offset; // 用于记录当前偏移量
	size_t _size; // 用于记录实际存储数据量
};
```
HVLBuf可用作可增长数组、双端队列

### 环形buffer：HRingBuf
```
class HRingBuf : public HBuf {
public:
	HRingBuf();
	HRingBuf(size_t cap);
	virtual ~HRingBuf();

	char* alloc(size_t len);
	void free(size_t len);
	void clear();
	size_t size();
	
private:
	size_t _head;
	size_t _tail;
	size_t _size;
};
```
HRingBuf初始化时分配一块大一点的内存，有序从中申请和释放，以避免频繁调用系统malloc/free(影响性能、造成内存碎片)。如用作音视频缓冲，解码线程不断申请内存生产帧数据，渲染线程不断消费帧数据并释放内存占用。

## hdef.h：常用宏定义
```c
#define ABS(n)  ((n) > 0 ? (n) : -(n)) // 绝对值
#define NABS(n) ((n) < 0 ? (n) : -(n)) // 负绝对值

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a))) // 数组大小

#define BITSET(p, n) (*(p) |= (1u << (n))) // 设置位
#define BITCLR(p, n) (*(p) &= ~(1u << (n))) // 清除位
#define BITGET(i, n) ((i) & (1u << (n))) // 获取位

#define CR      '\r' // mac换行符
#define LF      '\n' // unix换行符
#define CRLF    "\r\n" // dos换行符

#define IS_ALPHA(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z')) // 是否是字母
#define IS_NUM(c)   ((c) >= '0' && (c) <= '9') // 是否是数字
#define IS_ALPHANUM(c) (IS_ALPHA(c) || IS_NUM(c)) // 是否是字母或数字
#define IS_CNTRL(c) ((c) >= 0 && (c) < 0x20) // 是否是控制符
#define IS_GRAPH(c) ((c) >= 0x20 && (c) < 0x7F) // 是否是可打印字符
#define IS_HEX(c) (IS_NUM(c) || ((c) >= 'a' && (c) <= 'f') || ((c) >= 'A' && (c) <= 'F')) // 是否是16进制
#define IS_LOWER(c) (((c) >= 'a' && (c) <= 'z')) // 是否是小写
#define IS_UPPER(c) (((c) >= 'A' && (c) <= 'Z')) // 是否是大写

#define LOWER(c)    ((c) | 0x20) // 字符转小写
#define UPPER(c)    ((c) & ~0x20) // 字符转大写

#define MAX(a, b) ((a) > (b) ? (a) : (b)) // 两者取大
#define MIN(a, b) ((a) < (b) ? (a) : (b)) // 两者取小

#define SAFE_FREE(p)    do {if (p) {free(p); (p) = NULL;}} while(0) // 安全free
#define SAFE_DELETE(p)  do {if (p) {delete (p); (p) = NULL;}} while(0) // 安全delete
#define SAFE_DELETE_ARRAY(p) do {if (p) {delete[] (p); (p) = NULL;}} while(0) // 安全delete[]
```
以上列出一些最常用的，更多实用宏定义可自行浏览 https://github.com/ithewei/libhv/blob/master/base/hdef.h

## hdir.h：ls实现
hdir.h中目前只有listdir一个接口，使用简单。
测试代码见：
https://github.com/ithewei/libhv/blob/master/unittest/listdir_test.cpp
对跨平台ls实现感兴趣的可以阅读源码：
https://github.com/ithewei/libhv/blob/master/base/hdir.cpp
unix平台使用 opendir -> readdir -> closedir
windows平台使用FindFirstFile -> FindNextFile -> FindClose

## hendian.h：大小端
大小端与主机序转化宏（h代表主机序，be代表大端序，le代表小端序，数字代表多少位整型）
htobe16
htobe32
htobe64
be16toh
be32toh
be64toh
htole16
htole32
htole64
le16toh
le32toh
le64toh
detect_endian：检测大小端
serialize<T>：序列化模板函数，如序列化浮点数serialize<float>
deserialize<T>：反序列化模板函数，如反序列化浮点数deserialize<float>


## herr.h：错误码
herr.h中定义了一些错误码，用到了宏的映射技巧
接口只有一个hv_strerror：根据错误码获取错误字符串

## hexport.h：导出宏
HV_EXPORT：接口导出宏
HV_DEPRECATED：声明废弃宏
HV_UNUSED：声明未使用宏
EXTERN_C、BEGIN_EXTERN_C、END_EXTERN_C：c符号链接 extern "C" 相关宏
BEGIN_NAMESPACE、END_NAMESPACE、USING_NAMESPACE：c++命名空间相关宏

## hfile.h：文件类
提供了简单好用的HFile类
```c++
class HFile {
public:
	HFile() { fp = NULL; }
	~HFile() { close(); } // 析构时自动关闭文件
	
	int open(const char* filepath, const char* mode); // 打开文件（调用fopen）
	void close(); // 关闭文件（调用fclose）
	size_t read(void* ptr, size_t len); // 读文件（调用fread）
	size_t write(const void* ptr, size_t len) // 写文件（调用fwrite）
	size_t size(); // 返回文件大小（调用stat）

	size_t readall(HBuf& buf); // 读取文件所有内容到buffer
	size_t readall(std::string& str); // 读取文件所有内容到string
	bool readline(std::string& str); // 逐行读取，成功返回true，失败返回false
	
public:
	char  filepath[MAX_PATH];
	FILE* fp;
};
```

## hlog.h：日志
stdout_logger：标准输出日志
stderr_logger：标准错误日志
file_logger：文件日志
network_logger：网络日志（ 定义在 https://github.com/ithewei/libhv/blob/master/event/nlog.h ）
logger_create：创建日志器
logger_destroy：销毁日志器
logger_set_handler：设置日志处理函数
logger_set_level：设置日志等级
logger_set_level_by_str：设置日志等级by字符串[VERBOSE,DEBUG,INFO,WARN,ERROR,FATAL,SILENT]
logger_set_max_bufsize：设置日志缓存大小
logger_enable_color：启用日志颜色
logger_print：日志打印
logger_set_file：设置日志文件
logger_set_max_filesize：设置日志文件大小
logger_set_max_filesize_by_str：设置日志文件大小by字符串，如16, 16M, 16MB都表示16M
logger_set_remain_days：设置日志文件保留天数
logger_enable_fsync：启用每次写日志文件立即刷新到磁盘（即每次都调用fsync，会增加IO耗时，影响性能）
logger_fsync：刷新缓存到磁盘（如对日志文件实时性有必要的，可使用定时器定时刷新到磁盘）
logger_get_cur_file：获取当前日志文件路径

提供了默认的日志器hlog
```c
// macro hlog*
#define hlog                            hv_default_logger()
#define hlog_set_file(filepath)         logger_set_file(hlog, filepath)
#define hlog_set_level(level)           logger_set_level(hlog, level)
#define hlog_set_level_by_str(level)    logger_set_level_by_str(hlog, level)
#define hlog_set_max_filesize(filesize) logger_set_max_filesize(hlog, filesize)
#define hlog_set_max_filesize_by_str(filesize) logger_set_max_filesize_by_str(hlog, filesize)
#define hlog_set_remain_days(days)      logger_set_remain_days(hlog, days)
#define hlog_enable_fsync()             logger_enable_fsync(hlog, 1)
#define hlog_disable_fsync()            logger_enable_fsync(hlog, 0)
#define hlog_fsync()                    logger_fsync(hlog)
#define hlog_get_cur_file()             logger_get_cur_file(hlog)
```

提供了便利的日志宏hlogd, hlogi, hlogw, hloge, hlogf
提供了更大众化的别名LOGD, LOGI, LOGW, LOGE, LOGF
hlog跨平台、零依赖、多线程安全、使用简单、配置灵活；
即使你不使用libhv，也可以直接将hlog.h、hlog.c直接拷贝到你的项目中使用。

## hmain.h：命令行解析
main_ctx_init：main上下文初始化
parse_opt：解析命令行（类似于getopt）
parse_opt_long：解析命令行（类似于getopt_long）
get_arg：获取命令行参数值
get_env：获取环境变量
signal_init：信号初始化（内部初始化了SIGINT、SIGCHLD子进程崩溃重启、和自定义的SIGNAL_TERMINATE退出所以进程、SIGNAL_RELOAD重新加载配置文件）
signal_handle：信号处理signal=[start,stop,restart,status,reload]
create_pidfile：创建pid文件
delete_pidfile：删除pid文件
getpid_from_pidfile：从pid文件中获取pid
setproctitle：设置进程标题（只在unix下生效）
master_workers_run：master-workers模型，即多进程/多线程模型（参考nginx）  

hmain.h中提供一系列main入口有用的工具，如命令行解析、信号处理、创建pid文件、以及强大的master-workers模型。
测试代码见：https://github.com/ithewei/libhv/blob/master/examples/hmain_test.cpp

测试步骤：
```shell
make hmain_test # 编译hmain_test
bin/hmain_test -h # -h打印帮助信息
bin/hmain_test -v # -v打印版本信息
bin/hmain_test -c etc/hmain_test.conf -t # -c设置配置文件 -t测试加载配置文件
bin/hmain_test -d # -d后台运行
cat logs/hmain_test.pid # 可以看到创建的pid文件
ps aux | grep hmain_test # 可以看到master-workers模型效果
bin/hmain_test -s status # -s表示信号处理，status表示查看进程状态
bin/hmain_test -s stop # stop表示停止进程
bin/hmain_test -s start # start表示启动进程
bin/hmain_test -s restart -d # restart表示重启进程
bin/hmain_test -s reload # reload表示重新加载配置文件（比如修改日志等级为DEBUG，不用重启进程就能打印DEBUG日志了）
```
毫不夸张的说，搞懂了hmain_test.cpp，再也不用愁写不出规范的命令行程序了。

## hmath.h：数学函数
floor2e：2的指数倍向下取整，如floor2e(5) = 4
ceil2e：2的指数倍向上取整，如ceil2e(5) = 8
varint_encode: 可变长整型编码
varint_decode: 可变长整型解码

## hmutex.h：互斥锁
### 互斥锁
hmutex_init
hmutex_destroy
hmutex_lock
hmutex_unlock

### 自旋锁
hspinlock_init
hspinlock_destroy
hspinlock_lock
hspinlock_unlock

### 读写锁
hrwlock_init
hrwlock_destroy
hrwlock_rdlock
hrwlock_rdunlock
hrwlock_wrlock
hrwlock_wrunlock

### 定时锁
htimed_mutex_init
htimed_mutex_destroy
htimed_mutex_lock
htimed_mutex_unlock
htimed_mutex_lock_for

### 条件变量
hcondvar_init
hcondvar_destroy
hcondvar_wait
hcondvar_wait_for
hcondvar_signal
hcondvar_broadcast

### 信号量
hsem_init
hsem_destroy
hsem_wait
hsem_post
hsem_wait_for

### 只执行一次
honce

### c++类封装
MutexLock：互斥锁
SpinLock：自旋锁
RWLock：读写锁
LockGuard：守护锁（构造时即lock，析构时即unlock，类似于std::lock_guard）

### hthread
hthread.h跨平台，基于Windows API和pthread两套实现，命名类似pthread，无记忆负担，是不是很便利呢，妈妈再也不用担心没有c++，写不出跨平台的多线程同步代码了。

鄙人强烈推荐读下hthread.h源码，对应付面试中各种同步锁大有好处、加深理解。
测试代码见：https://github.com/ithewei/libhv/blob/master/unittest/hmutex_test.c

此处附上一个彩蛋：java synchronized 一行宏实现
```
#define synchronized(lock) for (std::lock_guard<std::mutex> _lock_(lock), *p = &_lock_; p != NULL; p = NULL)
```
测试代码见：https://github.com/ithewei/libhv/blob/master/unittest/synchronized_test.cpp

## hplatform.h：平台相关宏
操作系统宏：OS_WIN、OS_UNIX、OS_LINUX、OS_ANDROID、OS_DARWIN、OS_FREEBSD、OS_OPENBSD、OS_SOLARIS
体系结构宏：ARCH_X86、ARCH_X86_64、ARCH_ARM、ARCH_ARM64
编译器宏：COMPILER_MSVC、COMPILER_MINGW、COMPILER_GCC、COMPILER_CLANG
字节序宏：BYTE_ORDER、BIG_ENDIAN、LITTLE_ENDIAN
hplatform.h和hconfig.h（./configure脚本生成的配置文件）是libhv跨平台的基石。

## hproc.h：进程
hproc_spawn：unix下使用多进程，windows使用多线程

## hscope.h：作用域
ScopeCleanup：作用域清理函数
ScopeFree：作用域free
ScopeDelete：作用域delete
ScopeDeleteArray：作用域delete[]
ScopeRelease：作用域release
ScopeLock：作用域锁
hscope.h利用RAII机制，定义了一系列作用域模版类，方便做资源释放。

此处附上一个彩蛋：golang defer 宏实现
```c++
class Defer {
public:
	Defer(Function&& fn) : _fn(std::move(fn)) {}
	~Defer() { if(_fn) _fn();}
private:
	Function _fn;
};
#define defer(code) Defer STRINGCAT(_defer_, __LINE__)([&](){code});
```
defer测试代码见：https://github.com/ithewei/libhv/blob/master/unittest/defer_test.cpp

## hsocket.h：套接字
socket_errno：socket错误码
socket_strerror：根据socket错误码获取错误字符串
blocking：设置socket为阻塞
nonblocking：设置socket为非阻塞

### sockaddr_u：
包裹了IPv4、IPv6、Unix Domian Socket
```c
typedef union {
	struct sockaddr     sa;
	struct sockaddr_in  sin; // IPv4
	struct sockaddr_in6 sin6; // IPv6
	struct sockaddr_un  sun; // Unix Domain Socket
} sockaddr_u;
```
下面是一些sockaddr_u的辅助函数：
Resolver：域名解析成sockaddr_u
sockaddr_ip：从sockaddr_u中获取ip地址
sockaddr_port：从sockaddr_u中获取端口
sockaddr_set_ip：sockaddr_u设置ip地址
sockaddr_set_port：sockaddr_u设置端口
sockaddr_set_ipport：sockaddr_u设置ip地址和端口
sockaddr_set_path：sockaddr_u设置路径
sockaddr_len：sockaddr_u根据sa_family，获取结构体长度
sockaddr_str：sockaddr_u转化成可读字符串
sockaddr_print：sockaddr_u打印可读字符串

### socket、bind、listen、connect封装
Bind：封装了socket -> setsockopt -> bind流程
Listen：封装了Bind -> listen流程
Connect：封装了Resolver -> socket -> nonblocking -> connect流程
ConnectNonblock：非阻塞connect，调用了Connect(host, port, 1)
ConnectTimeout：超时connect，封装了Connect(host, port, 1) -> select -> blocking

### Unix Domain Socket（以路径代替端口号）
BindUnix
ListenUnix
ConnectUnix
ConnectUnixNonblock
ConnectUnixTimeout

### setsockopt
tcp_nodelay：禁用Nagle算法，降低小包的响应延时
tcp_nopush：当包累计到一定大小后再发送，通常与sendfile配合使用，提高大数据的通信性能
tcp_keepalive：设置TCP保活
udp_broadcast：设置UDP广播
so_sndtimeo：设置发送超时
so_rcvtimeo：设置接收超时
hsocket.h、hsocket.c：展示了跨平台socket编程的写法，适配了IPv4、IPv6、Unix Domain Socket，可以说是UNP（Unix Network Programming：UNIX网络编程）的实践，推荐网络编程初学者阅读源码。

## hssl.h：SSL/TLS加密通信
hssl_ctx_init：SSL_CTX初始化
hssl_ctx_cleanup：SSL_CTX清理
hssl_ctx_instance：返回SSL_CTX实例
hssl_new：SSL创建
hssl_free：SSL释放
hssl_accept
hssl_connect
hssl_read
hssl_write
hssl_close
hssl_set_sni_hostname：设置SNI域名
hssl.h封装了SSL/TLS操作，目前有openssl、gnutls、mbedtls、appletls等实现，编译时可选择打开WITH_OPENSSL、WITH_GNUTLS、WITH_MBEDTLS选项。

## hstring.h：字符串
hv::to_string：T转字符串模板函数
hv::from_string：字符串转T模板函数
asprintf：格式化输出字符串，如asprintf("%d+%d=%d", 1, 2, 3)返回字符串“1+2=3”
split：分割字符串成字符列表，如split("1, 2, 3")，返回字符串列表["1", "2", "3"]
splitKV：分割KV字符串，如splitKV("user=admin&pswd=123456")返回map{"user": "admin", "pswd": "123456"}
trim：修剪字符串
trimL：修剪字符串左边
trimR：修剪字符串右边
trim_pairs：指定pairs，修剪字符串
replace：替换字符串

## hpath.h: 路径
exists: 判断路径是否存在
isdir: 判断是否是目录
isfile: 判断是否是文件
islink: 判断是否是链接
basename：获取基本路径，类似shell命令basename
dirname：获取目录名，类似shell命令dirname
filename：获取文件名
suffixname：获取后缀名
join: 路径拼接
例如：
```c
std::string filepath = "/mnt/share/image/test.jpg";
basename(filepath) = "test.jpg"
dirname(filepath) = "/mnt/share/image"
filename(filepath) = "test"
suffixname(filepath) = "jpg"
```
测试代码见：https://github.com/ithewei/libhv/blob/master/unittest/hstring_test.cpp

## hsysinfo.h：系统信息
get_ncpu：获取CPU逻辑核数
get_meminfo：获取内存信息

## hthread.h：线程
hv_getpid：获取进程id
hv_gettid：获取线程id
hthread_create：创建线程
hthread_join：加入线程（等待线程退出）
c++提供了一个HThread线程封装类
```c++
class HThread {
public:
	virtual int start(); // 开始
	virtual int stop(); // 结束
	virtual int pause(); // 暂停
	virtual int resume(); // 继续
	virtual void run(); // 可重载run自定义线程过程
	virtual void doTask(); // 可重载doTask实现线程循环中的任务函数
};
```
hthread.h封装了跨平台的线程操作，在不使用c++ std::thread的情况下也能写出跨平台的创建线程程序了。

## hthreadpool.h：线程池
```c++
class HThreadPool {
public:
	HThreadPool(int size = std::thread::hardware_concurrency()); // size为线程池中线程数量
	int start(); // 开始
	int stop(); // 停止
	int pause(); // 暂停
	int resume(); // 继续

	int wait(); // 等待所有任务做完
	// commit：提交任务
	// return a future, calling future.get() will wait task done and return RetType.
	// commit(fn, args...)
	// commit(std::bind(&Class::mem_fn, &obj))
	// commit(std::mem_fn(&Class::mem_fn, &obj))
	template<class Fn, class... Args>
	auto commit(Fn&& fn, Args&&... args) -> std::future<decltype(fn(args...))>
};
```
测试代码见：https://github.com/ithewei/libhv/blob/master/unittest/threadpool_test.cpp

## htime.h：时间日期
htime.h、htime.c封装了跨平台的日期时间方法，接口如下：

gettick_ms：返回tick毫秒数
gettimeofday_ms：返回gettimeofday毫秒数
gethrtime_us：返回高精度微秒数
datetime_now：返回日期时间
datetime_mktime：根据datetime_t返回时间戳
datetime_past：返回过去几天前的datetime_t
datetime_future：返回未来几天后的datetime_t
duration_fmt：时长格式化成字符串
datetime_fmt：datetime_t格式化成字符串
gmtime_fmt：GMT时间格式化成字符串
days_of_month：返回某月有多少天
month_atoi：月份字符串转整数months=["January", "February", "March", "April", "May", "June","July", "August", "September", "October", "November", "December"]
month_itoa：月份整数转字符串
weekday_atoi：星期字符串转整数weekdays=["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]
weekday_itoa：星期整数转字符串
hv_compile_datetime：返回libhv库的编译日期时间
cron_next_timeout：计算cron下一次触发时间
测试代码见：https://github.com/ithewei/libhv/blob/master/unittest/date_test.c

## hurl.h：URL相关
url_escape：URL转义（将URL中有歧义字符如/，转化成%02X16进制格式）
url_unescape：URL反转义

## hversion.h：版本
hv_version：返回libhv静态版本
hv_compile_version：返回libhv编译版本
version_atoi：版本字符串转整型（如1.2.3.4 => 0x01020304）
version_itoa：版本整型转字符串（如0x01020304 => 1.2.3.4）

## hv.h：总头文件
base下的头文件比较分散，所以提供了一个hv.h的总头文件

## ifconfig.h：ifconfig实现
ifconfig：获取网络接口信息，类似shell下ifconfig命令
测试代码见：https://github.com/ithewei/libhv/blob/master/unittest/ifconfig_test.cpp

# event事件循环
event模块是libhv的事件循环模块，实现了类似libevent、libev、libuv非阻塞IO、定时器等功能。
如果说base模块是libhv的基石，event模块就是libhv的灵魂，上层的应用层协议（如http、websocket、redis、mqtt）都将基于event模块搭建。

## hloop.h：事件循环
hloop_new：创建事件循环实例
hloop_free：释放事件循环实例
hloop_run：运行事件循环
hloop_stop：停止事件循环
hloop_pause：暂停事件循环
hloop_resume：继续事件循环
hloop_update_time：更新事件循环时间
hloop_now：返回事件循环里的当前时间（单位s）
hloop_now_ms：返回事件循环里的当前时间（单位ms）
hloop_now_us：返回事件循环里的当前时间（单位us）
### 用户数据
hloop_set_userdata：设置用户数据
hloop_userdata：返回用户数据
### 自定义事件
hloop_post_event：向事件循环投递自定义事件（此接口线程安全）

## 事件基类
hio_t、htimer_t、hidle_t皆是hevent_t的子类，继承hevent_t数据成员以及函数成员
hevent_set_priority：设置事件优先级
hevent_set_userdata：设置事件用户数据
hevent_loop：返回当前loop
hevent_type：返回事件类型
hevent_id：返回事件id
hevent_priority：返回事件优先级
hevent_userdata：返回事件用户数据

### idle空闲事件
hidle_add：添加空闲事件回调
hidle_del：删除空闲事件

### timer定时器事件
htimer_add：添加定时器
htimer_add_period：添加period型定时器（类似于cron）
htimer_del：删除定时器
htimer_reset：重置定时器

### IO事件
#### low-level apis
hio_engine：返回底层IO多路复用引擎（select/poll/epoll/kqueue/iocp/evport）
hio_get：根据sockfd返回一个hio_t实例指针
hio_add：添加IO读写事件
hio_del：删除IO读写事件
hio_fd：返回文件描述符
hio_error：返回错误码
hio_type：返回IO类型（STDIO/FILE/TCP/UDP/SSL等）
hio_localaddr：返回本端地址
hio_peeraddr：返回对端地址
hio_setcb_accept：设置accept回调
hio_setcb_connect：设置connect回调
hio_setcb_read：设置read回调
hio_setcb_write：设置write回调
hio_setcb_close：设置close回调
hio_enable_ssl：启用SSL/TLS
hio_set_readbuf：设置读缓存buffer（每一个事件循环占用一个线程，所以事件循环中有一个默认的读缓存，但是你可以通过此接口传入自己的buffer，避免后续再memcpy拷贝）
hio_set_connect_timeout：设置连接超时
hio_set_close_timeout：设置关闭超时（这里解释下为何非阻塞IO有关闭超时一说，因为可能存在写队列未发送完成的情况，所以需要延迟关闭套接字）
hio_set_read_timeout：设置读超时，一段时间没有数据接收，自动断开连接
hio_set_write_timeout：设置写超时，一段时间没有数据发送，自动断开连接
hio_set_keepalive_timeout：设置keepalive超时（一段时间无数据收发断开连接，http模块即用到了此接口，使用nc 127.0.0.1 8080连接后不发数据，75s后连接将被libhv httpd服务端强制断开）
hio_set_heartbeat：设置应用层心跳
hio_set_unpack：设置拆包规则，支持固定包长、分隔符、头部长度字段三种常见的拆包方式，内部根据拆包规则处理粘包与分包，保证回调上来的是完整的一包数据，大大节省了上层处理粘包与分包的成本
hio_read_until_length：读取数据直到指定长度才回调上来
hio_read_until_delim：读取数据直到遇到分割符才回调上来
hio_setup_upstream：设置转发
hio_accept
hio_connect
hio_read
hio_write
hio_close

### high-level apis
hread
hwrite
hclose
tcp
haccept
hconnect
hrecv
hsend
udp
hio_set_type
hio_set_localaddr
hio_set_peeraddr
hrecvfrom
hsendto

### top-level apis
hloop_create_tcp_server：在事件循环中创建TCP服务端
hloop_create_tcp_client：在事件循环中创建TCP客户端
hloop_create_udp_server：在事件循环中创建UDP服务端
hloop_create_udp_client：在事件循环中创建UDP客户端

## 总结
hloop.h是事件循环对外头文件，如想使用libhv开发TCP/UDP自定义协议网络通信程序，建议通读此头文件。

### 实现
event模块封装了多种IO多路复用机制，感兴趣的可以阅读源码，你将对reactor模式、select/poll/epoll有更深的理解。

linux下默认使用epoll
windows下使用poll（IOCP尚不完善）
mac下使用kqueue
solaris下使用port

### 测试代码见：
定时器测试 https://github.com/ithewei/libhv/blob/master/examples/htimer_test.c
TCP服务端 https://github.com/ithewei/libhv/blob/master/examples/tcp_echo_server.c
UDP服务端 https://github.com/ithewei/libhv/blob/master/examples/udp_echo_server.c
TCP/UDP客户端 https://github.com/ithewei/libhv/blob/master/examples/nc.c

### 测试步骤：
examples下展示了用libhv实现echo、chat、proxy三种经典服务的写法，使用前event模块的必读。
```bash
make examples

# 回显
bin/tcp_echo_server 1234
bin/nc 127.0.0.1 1234

# 群聊
bin/tcp_chat_server 1234
bin/nc 127.0.0.1 1234
bin/nc 127.0.0.1 1234

# 代理
bin/httpd -s restart -d
bin/tcp_proxy_server 1234 127.0.0.1:8080
bin/curl -v 127.0.0.1:8080
bin/curl -v 127.0.0.1:1234

bin/udp_echo_server 1234
bin/nc -u 127.0.0.1 1234
```

## nlog.h：网络日志
network_logger：网络日志处理器
nlog_listen：网络日志监听服务
测试代码见：https://github.com/ithewei/libhv/blob/master/examples/hloop_test.c
测试步骤：
```
make examples
bin/hloop_test
telnet 127.0.0.1 10514
```

## nmap.h：nmap实现
nmap_discover：nmap实现
segment_discover：网段发现
host_discover：主机发现
测试代码见：https://github.com/ithewei/libhv/blob/master/examples/nmap
测试步骤：
```shell
make nmap
sudo bin/nmap <ip>
```

## utils工具
### base64.h：base64编解码
hv_base64_encode：base64编码
hv_base64_decode：base64解码

### md5.h：MD5数字摘要
HV_MD5Init：MD5初始化
HV_MD5Update：MD5更新
HV_MD5Final：MD5结束

### sha1.h：SHA1安全散列算法
HV_SHA1Init：SHA1初始化
HV_SHA1Update: SHA1更新
HV_SHA1Final：SHA1结束

### iniparser.h：ini解析
```c++
class HV_EXPORT IniParser {
public:
    int LoadFromFile(const char* filepath); // 从文件加载
    int LoadFromMem(const char* data); // 从内存加载
    int Unload(); // 卸载
    int Reload(); // 重新加载

    string DumpString(); // 转存为字符串
    int Save(); // 保存
    int SaveAs(const char* filepath); // 保存为

    string GetValue(const string& key, const string& section = ""); // 获取值
    void   SetValue(const string& key, const string& value, const string& section = ""); // 设置值

    // T = [bool, int, float]
    template<typename T>
    T Get(const string& key, const string& section = "", T defvalue = 0); // 获取值模板函数

    // T = [bool, int, float]
    template<typename T>
    void Set(const string& key, const T& value, const string& section = ""); // 设置值模板函数
};
```
### json.hpp：json解析
nlohmann::json是一个modern c++ json解析库，具体使用参考https://github.com/nlohmann/json
（注：windows下该头文件需在VS2015以上版本才能顺利编译通过）

### singleton.h：单例模式宏
DISABLE_COPY：禁止拷贝宏
SINGLETON_DECL：单例模式声明宏
SINGLETON_IMPL：单例模式实现宏

## http协议
http模块的使用可参考这篇博客c++编写HTTP API服务端/客户端最简单的库，没有之一

http模块是event模块目前仅有的得意门生，服务端接口参考了golang gin，客户端接口参考了python requests，使用c++开发，至于为何使用c++，如果你用过libevent的evhttp，就知道c写上层逻辑是有多痛苦。
我也不推荐使用libhv开发CRUD业务服务，使用golang、python、ruby它不香吗？c++开发http服务的场景可能就是需要将C接口的算法SDK以http api的方式提供出去。

### HttpMessage.h：http消息类
HttpMessage：http消息基类
HttpRequest：http请求类
HttpResponse：http响应类
HttpMessage.h建议通读，才能更好的使用好libhv的http相关功能。

### HttpServer.h：http服务端
http_server_run
http_server_stop
class HttpServer
测试代码见：https://github.com/ithewei/libhv/tree/master/examples/httpd
测试步骤：
./getting_started.sh

### http_client.h：http客户端
http_client_send：同步客户端
http_client_send_async：异步客户端
测试代码见：https://github.com/ithewei/libhv/blob/master/examples/curl.cpp
测试步骤：
```
make curl
bin/curl -v www.example.com
```
另外有python requests风格requests.h、javascript axios风格axios.h的封装。

## websocket协议
### WebSocketServer.h: websocket服务端
websocket_server_run
websocket_server_stop
class WebSocketServer
测试代码见：https://github.com/ithewei/libhv/tree/master/examples/websocket_server_test.cpp

### WebSocketClient.h: websocket客户端
class WebSocketClient
测试代码见：https://github.com/ithewei/libhv/tree/master/examples/websocket_client_test.cpp

## protocol各种常见协议
protocol模块展示了各种常见协议的实现，可供学习参考。

### icmp.h：ping实现
ping：ping实现
测试代码见：https://github.com/ithewei/libhv/blob/master/unittest/ping_test.c
测试步骤：
```
make unittest
sudo bin/ping www.baidu.com
```
### dns.h：DNS域名查找
nslookup：dns查找
测试代码见：https://github.com/ithewei/libhv/blob/master/unittest/nslookup_test.c
测试步骤：
```
make unittest
bin/nslookup www.baidu.com
```

### ftp.h：FTP文件传输协议
ftp_connect：连接
ftp_login：登录
ftp_quit：退出
ftp_exec：执行命令
ftp_upload：上传文件
ftp_download：下载文件
测试代码见：https://github.com/ithewei/libhv/blob/master/unittest/ftp_test.c

### smtp.h：SMTP邮件传输协议
sendmail：发送邮件
测试代码见：https://github.com/ithewei/libhv/blob/master/unittest/sendmail_test.c

# last
个人开发维护实属不易，如果觉得不错，请github上star下，若libhv能带给你一点启发，吾将甚感欣慰！
