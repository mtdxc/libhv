`libhv`是一个比`libevent、libev、libuv`更易用的跨平台国产网络库，可用来开发`TCP/UDP/SSL/HTTP/WebSocket`客户端/服务端。

## Q：libhv名称由来
A：`libhv`是一个类似于`libevent、libev、libuv`的跨平台网络库，提供了带非阻塞IO和定时器的事件循环。
`libhv`的名称也正是继承此派，寓意高性能的事件循环`High-performance event loop library`。

## Q：libhv和libevent、libev、libuv有什么不同
A：
- libevent最为古老、有历史包袱，bufferevent虽为精妙，却也难以理解使用；
- libev可以说是libevent的简化版，代码极为精简，但宏定义用的过多，代码可读性不强，且在Windows上实现不佳；
- libuv是nodejs的c底层库，最先也是由libevent+对Windows IOCP支持，后来才改写自成一体，同时实现了管道、文件的异步读写，很强大，但结构体比较多，封装比较深，`uv_write`个人感觉难用；
- libhv本身是参考了libevent、libev、libuv的实现思路，它们的核心都是事件循环（即在一个事件循环中处理IO、定时器等事件），但提供的接口最为精简，API接近原生系统调用，最容易上手；
- 具体这几个库的写法比较见<https://github.com/ithewei/libhv/tree/master/echo-servers>
- 此外libhv集成了SSL/TLS加密通信，支持心跳、转发、拆包、多线程安全write和close等特性，实现了HTTP、WebSocket等协议；
- 当然这几个库的性能是接近的，都将非阻塞IO多路复用用到了极致；
- 更详细介绍见[国产开源库libhv为何能被awesome-c和awesome-cpp收录](https://hewei.blog.csdn.net/article/details/104920336)

## Q：libhv的定位
A：`精妙小巧跨平台，简单实用易上手`
- base封装了很多跨平台的代码，如hatomic原子操作、hthread线程、hmutex线程同步，当然这都是基于`configure`/`cmake`自动生成的`hconfig.h`和`hplatform.h`两个头文件中提供的平台宏、编译器宏等实现的；
- event模块则实现了事件循环（包括IO、timer、idle），不同的平台有不同的实现，如`Linux`使用`epoll`，`Windows`使用`IOCP`、`Mac`使用`kqueue`、`Solaris`使用`evport`，感兴趣的可以读一读event下的源码；
- http模块则基于event模块实现了本世纪最为通用的应用层协议http协议，包括http服务端和客户端，libhv中examples下提供的httpd，性能可媲美nginx服务；
- 不妨勇敢的说，[libhv是c++编写HTTP API服务端/客户端最简单的库，没有之一](https://hewei.blog.csdn.net/article/details/104055509)

##  Q：libhv的发展规划
A：基于event模块实现更多的常见应用层协议，如`MQTT`、`redis`、`kafka`、`mysql`等；<br>
更多发展规划详见[docs/PLAN.md](https://github.com/ithewei/libhv/blob/master/docs/PLAN.md)

## Q：libhv性能如何
A：
- 吞度量测试：在echo-servers目录下有测试脚本，大致结果 `libev = libhv > libuv > libevent = asio = POCO`
- HTTP压测：相同进程数下，nginx采用默认配置，libhv httpd的QPS约等于nginx的1.5倍
- 测试步骤：https://gitee.com/libhv/libhv/blob/master/.github/workflows/benchmark.yml
- Github Action测试结果数据：https://github.com/ithewei/libhv/actions/workflows/benchmark.yml

## Q：libhv稳定性如何，是否商用
A：libhv自2018年5月创建，至今已有三年多迭代，700+提交，广泛用于公司IoT和HTTP API服务中，此外QQ群里也是有不少水友成功用在各种项目中，反馈很好；
请放心使用，开源且保证长期维护，QQ群里也有很多大神积极解答。

## Q：libhv如何入门
A：
- 建议先从运行项目根目录下`getting_started.sh`脚本开始， 你会被libhv的httpd所展示的便利性所吸引；
- 阅读libhv入门教程：<https://hewei.blog.csdn.net/category_9866493.html>
- 看`examples`下的示例代码；
- 源码阅读推荐路线`base->event->http`；

## Q：libhv如何使用
A：libhv可通过`Makefile`或`cmake`编译出动态库和静态库，`make install`后包含相关头文件（base模块下头文件比较分散，可直接`#include "hv.h"`）和链接库文件即可使用；当然libhv模块划分清晰，低耦合，你也可以直接把源文件拿到自己项目中去编译，如日志功能`hlog.h`和`hlog.c`就可以直接拿去用。

## Q：libhv如何交叉编译
A：以ubuntu下编译arm为例：

Makefile方式：
```shell
sudo apt install gcc-arm-linux-gnueabi g++-arm-linux-gnueabi
export CROSS_COMPILE=arm-linux-gnueabi-
./configure
make clean
make libhv
```
cmake方式：
```shell
mkdir build
cd build
cmake .. -DCMAKE_C_COMPILER=arm-linux-gnueabi-gcc -DCMAKE_CXX_COMPILER=arm-linux-gnueabi-g++
cmake --build . --target libhv libhv_static
```
更多编译平台和编译选项介绍见[BUILD.md](https://gitee.com/libhv/libhv/blob/master/BUILD.md)

## Q：libhv在Windows下如何编译
A：Windows下编译libhv请先使用`cmake`生成VS工程。
附VS各版本下载地址[VS2008 ~ VS2019下载地址](https://hewei.blog.csdn.net/article/details/102487918)
[cmake官网](https://cmake.org/download/)下载过慢的可以到`gitee`下载`cmake release`包<https://gitee.com/ithewei/cmake-release>
<font color=red>cmake不会使用的请自行百度</font>

## Q：Windows下编译不过
A：Windows下VS编译最低要求VS2015（包括VS2015）版本，这是因为http模块中使用了一个modern c++ JSON解析库[nlohmann::json](https://github.com/nlohmann/json)，该json库使用方法见<https://github.com/nlohmann/json>

如果想使用vs低版本编译或只使用c语言的，可以在cmake时关闭使用了`c++11`的模块`WITH_EVPP`、`WITH_HTTP`，只编译base、event等c模块。

## Q：Windows下链接不过
A: Windows下cmake生成vs工程，打开`hv.sln`编译后会生成`头文件include/hv、静态库lib/hv_static.lib和动态库lib/hv.dll`，所以有动态库和静态库两种链库方式：

### 1、动态导入库hv.lib + 动态库hv.dll
方案一：工程-> 属性 -> Linker -> Input -> Addtional Dependencies 加 `hv.lib`
方案二：代码里添加`#pragma comment(lib, "hv.lib")`
	
### 2、静态库声明宏HV_STATICLIB + 静态库hv_static.lib
- 工程-->属性-->`c/c++`-->预处理器-->预处理器定义中添加`HV_STATICLIB`预编译宏，以屏蔽`hexport.h`头文件中动态库导入宏`#define HV_EXPORT  __declspec(dllimport)`
如使用curl静态库类似加`CURL_STATICLIB`预编译宏
- 工程-> 属性 -> Linker -> Input -> Addtional Dependencies 加 `hv_static.lib` 或
  代码里添加`#pragma comment(lib, "hv_static.lib")`

## Q：如何开启SSL/TLS、https、wss功能
A：`libhv`中集成了`openssl`来支持`SSL/TLS`加密通信，通过打开`config.mk`或`CMakeList.txt`中`WITH_OPENSSL`选项，编译即可。

Makefile方式：
```shell
./configure --with-openssl
make clean && make && sudo make install
```
cmake方式：
```shell
mkdir build
cd build
cmake .. -DWITH_OPENSSL=ON
cmake --build .
sudo cmake --install .
```

测试https：
```shell
bin/httpd -s restart -d
bin/curl -v http://localhost:8080
bin/curl -v https://localhost:8443
# curl -v https://127.0.0.1:8443 --insecure
```

https代码示例可以参考[examples/http_server_test.cpp](https://gitee.com/libhv/libhv/blob/master/examples/http_server_test.cpp)中`TEST_HTTPS`相关内容<br>
wss代码示例可以参考[examples/websocket_server_test.cpp](https://gitee.com/libhv/libhv/blob/master/examples/websocket_server_test.cpp)中`TEST_WSS`相关内容

当然你也可以用`nginx`做`https`代理。

## Q：Windows下如何集成openssl
A：Windows下请自行下载或编译`openssl`，将`openssl`头文件`include`和库文件`lib`放到libhv可搜索路径（如`libhv`根目录下`include`和`lib`）。

附`gitee`上`Windows openssl`已编译好的<https://gitee.com/ithewei/openssl-release.git> 
（需将`libssl.dll.a改名为ssl.lib`，`libcrypto.dll.a改名为crypto.lib`）

## Q：https/wss连接失败排除步骤
1、确认是否已集成`SSL/TLS`库

以集成`openssl`为例，确认方法如下：

- linux下可使用`ldd libhv.so`，查看动态库依赖项中是否有`libssl.so、libcrypto.so`
- windows下使用命令行工具`dumpbin /DEPENDENTS hv.dll`或者图形界面工具`dependency`查看
- 代码里可打印`hssl_backend()`，如打印`openssl`则表示使用了`openssl`

2、连接失败后，日志里查看是否有`ssl handshake failed`失败的字样，如有表示开启了SSL，但是握手失败，具体原因可能是对端开启了证书验证，需要调用`hssl_ctx_init`输入有效的证书。

接口定义：
```c
typedef struct {
    const char* crt_file;
    const char* key_file;
    const char* ca_file;
    const char* ca_path;
    short       verify_peer;
    short       endpoint;
} hssl_ctx_init_param_t;

HV_EXPORT hssl_ctx_t hssl_ctx_init(hssl_ctx_init_param_t* param);
```
调用示例：
```c
    hssl_ctx_init_param_t param;
    memset(&param, 0, sizeof(param));
    param.crt_file = "cert/server.crt";
    param.key_file = "cert/server.key";
    if (hssl_ctx_init(&param) == NULL) {
        fprintf(stderr, "hssl_ctx_init failed!\n");
        return -20;
    }
```

## Q：http如何上传、下载文件
A:

上传文件：
- 只上传文件，设置`Content-Type`，如`image/jpeg`，将文件内容读入body即可，见`HttpMessage::File、requests::uploadFile`接口；
- 上传文件+其它参数，推荐使用formdata格式，即`Content-Type: multipart/form-data`，见`HttpMessage::FormFile、requests::uploadFormFile`接口；
- 如不得不使用json格式，需将二进制文件base64编码后赋值给body；

下载文件：
- httpd服务自带静态资源服务，设置`document_root`，即可通过url下载该目录下的文件，如`wget http://ip:port/path/to/filename`
- 下载大文件推荐使用Range头分片请求，具体参考[examples/wget.cpp](https://gitee.com/libhv/libhv/blob/master/examples/wget.cpp)

## Q: http如何异步响应
A：编写http服务端，强烈建议通读[examples/httpd](https://gitee.com/libhv/libhv/tree/master/examples/httpd)，里面有你想要的一切

- 异步响应参考`/async`;
- 定时响应参考`Handler::setTimeout`;
- json响应参考`Handler::json`;
- formdata响应参考`Handler::form`;
- urlencoded响应参考`Handler::kv`;
- restful风格参考`Handler::restful`

```cpp
// 同步handler: 适用于非阻塞型的快速响应
typedef std::function<int(HttpRequest* req, HttpResponse* resp)>                            http_sync_handler;
// 异步handler: 适用于耗时处理和响应
typedef std::function<void(const HttpRequestPtr& req, const HttpResponseWriterPtr& writer)> http_async_handler;
// 类似nodejs koa的ctx handler: 兼容以上两种handler的最新写法，可在回调里自己决定同步响应还是异步响应
typedef std::function<int(const HttpContextPtr& ctx)>                                       http_ctx_handler;
```
因为历史兼容原因，同时保留支持以上三种格式的`handler`，用户可根据自己的业务和接口耗时选择合适的`handler`，如果使用的较新版`libhv`，推荐使用带`HttpContext`参数的`http_ctx_handler`。

三种`handler`的等同写法见：
```cpp
    // 同步handler: 回调函数运行在IO线程
    router.POST("/echo", [](HttpRequest* req, HttpResponse* resp) {
        resp->content_type = req->content_type;
        resp->body = req->body;
        return 200;
    });

    // 异步handler：回调函数运行在hv::async全局线程池
    router.POST("/echo", [](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
        writer->Begin();
        writer->WriteStatus(HTTP_STATUS_OK);
        writer->WriteHeader("Content-Type", req->GetHeader("Content-Type"));
        writer->WriteBody(req->body);
        writer->End();
    });

    // 带HttpContext参数的handler是兼容同步/异步handler的最新写法，推荐使用
    // 回调函数运行在IO线程，可通过hv::async丢到全局线程池处理，或者自己的消费者线程/线程池
    // HttpContext里包含了HttpRequest和HttpResponseWriter成员变量，参照nodejs koa提供了一系列操作HttpRequest和HttpResponse的成员函数，写法更加简洁
    router.POST("/echo", [](const HttpContextPtr& ctx) {
        return ctx->send(ctx->body(), ctx->type());
    });

    router.POST("/echo", [](const HttpContextPtr& ctx) {
        // demo演示丢到hv::async全局线程池处理，实际使用推荐丢到自己的消费者线程/线程池
        hv::async([ctx]() {
            ctx->send(ctx->body(), ctx->type());
        });
        return 0;
    });
```
**Tips:**

- `std::async`在不同c++运行库下有着不同的实现，有的是线程池，有的就是当场另起一个线程，而且返回值析构时也会阻塞等待，不推荐使用，可以使用`hv::async`代替（需要#`include “hasync.h”`），可以通过`hv::async::startup`配置全局线程池的最小线程数、最大线程数、最大空闲时间，`hv::async::cleanup`用于销毁全局线程池；
- 关于是否需要丢到消费者线程处理请求的考量：在并发不高的场景，通过设置`worker_threads`起多线程就可以满足了，不能满足的（并发很高不能容忍阻塞后面请求、handler回调里耗时秒级以上）才考虑将`HttpContextPtr`丢到消费者线程池处理；
- 关于大文件的发送可以参考 [examples/httpd](https://gitee.com/libhv/libhv/blob/master/examples/httpd/handler.cpp) 里的`largeFileHandler`，单独起线程`循环读文件->发送`，但是要注意做好流量控制，因为磁盘IO总是快于网络IO的，或者对方接受过慢，都会导致发送数据积压在发送缓存里，耗费大量内存，示例里是通过判断`WriteBody`返回值调整`sleep`睡眠时间从而控制发送速度的，当然你也可以通过`hio_fd(ctx->writer->io())`获取到套接字，设置成阻塞来发；或者设置`ctx->writer->onwrite`监听写完成事件统计写数据来决定是否继续发送；或者通过`hio_write_bufsize`获取当前写缓存积压字节数来决定是否继续发送；
- 关于发送事先不知道长度的实时流数据，可以通过`chunked`方式，回调里基本流程是`Begin -> EndHeaders("Transfer-Encoding", "chunked") -> WriteChunked -> WriteChunked -> ... -> End`

## Q: TCP如何处理粘包与分包
A：libhv提供了设置拆包规则接口，c接口见`hio_set_unpack`，c++接口见`SocketChannel::setUnpack`，支持`固定包长、分隔符、头部长度字段`三种常见的拆包方式，调用该接口设置拆包规则后，内部会根据拆包规则处理粘包与分包，保证回调上来的是完整的一包数据，大大节省了上层处理粘包与分包的成本，该接口具体定义如下：
```c
typedef enum {
    UNPACK_BY_FIXED_LENGTH  = 1,    // 根据固定长度拆包
    UNPACK_BY_DELIMITER     = 2,    // 根据分隔符拆包，如常见的“\r\n”
    UNPACK_BY_LENGTH_FIELD  = 3,    // 根据头部长度字段拆包
} unpack_mode_e;

#define DEFAULT_PACKAGE_MAX_LENGTH  (1 << 21)   // 2M

// UNPACK_BY_DELIMITER
#define PACKAGE_MAX_DELIMITER_BYTES 8

// UNPACK_BY_LENGTH_FIELD
typedef enum {
    ENCODE_BY_VARINT        = 1,                // varint编码
    ENCODE_BY_LITTEL_ENDIAN = LITTLE_ENDIAN,    // 小端编码
    ENCODE_BY_BIG_ENDIAN    = BIG_ENDIAN,       // 大端编码
} unpack_coding_e;

typedef struct unpack_setting_s {
    unpack_mode_e   mode; // 拆包模式
    unsigned int    package_max_length; // 最大包长度限制
    // UNPACK_BY_FIXED_LENGTH
    unsigned int    fixed_length; // 固定包长度
    // UNPACK_BY_DELIMITER
    unsigned char   delimiter[PACKAGE_MAX_DELIMITER_BYTES]; // 分隔符
    unsigned short  delimiter_bytes; // 分隔符长度
    // UNPACK_BY_LENGTH_FIELD
    unsigned short  body_offset; // body偏移量（即头部长度）real_body_offset = body_offset + varint_bytes - length_field_bytes
    unsigned short  length_field_offset; // 头部长度字段偏移量
    unsigned short  length_field_bytes; // 头部长度字段所占字节数
    unpack_coding_e length_field_coding; // 头部长度字段编码方式，支持varint、大小端三种编码方式，通常使用大端字节序（即网络字节序）
#ifdef __cplusplus
    unpack_setting_s() {
        // Recommended setting:
        // head = flags:1byte + length:4bytes = 5bytes
        mode = UNPACK_BY_LENGTH_FIELD;
        package_max_length = DEFAULT_PACKAGE_MAX_LENGTH;
        fixed_length = 0;
        delimiter_bytes = 0;
        body_offset = 5;
        length_field_offset = 1;
        length_field_bytes = 4;
        length_field_coding = ENCODE_BY_BIG_ENDIAN;
    }
#endif
} unpack_setting_t;

HV_EXPORT void hio_set_unpack(hio_t* io, unpack_setting_t* setting);
```
以`ftp`为例（分隔符方式）可以这样设置：
```c
unpack_setting_t ftp_unpack_setting;
memset(&ftp_unpack_setting, 0, sizeof(unpack_setting_t));
ftp_unpack_setting.package_max_length = DEFAULT_PACKAGE_MAX_LENGTH;
ftp_unpack_setting.mode = UNPACK_BY_DELIMITER;
ftp_unpack_setting.delimiter[0] = '\r';
ftp_unpack_setting.delimiter[1] = '\n';
ftp_unpack_setting.delimiter_bytes = 2;
```
以`mqtt`为例（头部长度字段方式）可以这样设置：
```c
unpack_setting_t mqtt_unpack_setting = {
    .mode = UNPACK_BY_LENGTH_FIELD,
    .package_max_length = DEFAULT_PACKAGE_MAX_LENGTH,
    .body_offset = 2,
    .length_field_offset = 1,
    .length_field_bytes = 1,
    .length_field_coding = ENCODE_BY_VARINT,
};
```
具体实现代码在[event/unpack.c](https://github.com/ithewei/libhv/blob/master/event/unpack.c)中，在内部`readbuf`的基础上直接原地拆包与组包，基本做到零拷贝，比抛给上层处理更高效，感兴趣的可以研究一下。

具体示例可参考[examples/jsonrpc](https://github.com/ithewei/libhv/tree/master/examples/jsonrpc)、[examples/protorpc](https://github.com/ithewei/libhv/tree/master/examples/protorpc)

## Q：c++已经跨平台，base模块为何要封装跨平台操作
A：

1、c++标准库提取的是所有操作系统的共性，所以它甚至不能像其它语言（没有操作系统包袱，只需要满足主流操作系统）那样提供通用的时间日期操作，也没有提供差异化的锁（自旋锁、读写锁），你可以发现java中锁的类型一大堆，而c++只有一个`mutex`；至于没有提供标准网络库，更是c++一直被诟病之处。

2、`event`模块是纯c实现的，`libevent、libuv`也是如此，底层库使用c++性能有损、库大小、复杂度也会增加，并不会带来编码上的简化。如果只把libhv当作`libevent`来使用，关闭`WITH_HTTP`选项，是可以做到不依赖`stdc++`的。`event`模块本身也是封装了各种操作系统的IO多路复用机制（如`linux的epoll`、`bsd的kqueue`、`通用的select、poll`等），提供出了统一的非阻塞IO接口。

3、http模块使用c++的考量，是为了接口使用上的便利性（`HttpRequest`、`HttpResponse`中使用了`map、string`来表示`headers、body`，`json、form、kv`来存储各种`Content-Type`解析后的结构化数据，`Get、Set`模板函数屏蔽了`int、float、string`之间的类型转化），你如果使用过`libevent`的`evhttp`就会发现，c写这些会非常痛苦。

4、没有任何贬低或者褒奖c、c++，归根结底它们只是有各自特色的编程语言，只是你实现业务的工具，避其糟粕、用其精华、为你所有，才是其价值。

如果你是写数据库的`CRUD`应用，提供http api服务，我也并不推荐使用libhv，使用`golang、python、ruby`它不香吗？`c++ http`库使用场景可能就是需要将`c接口SDK`的算法功能以http api服务的方式提供出去。

## Q：libhv提倡的编程范式？
A：
c/c++本身是一种支持多编程范式的语言，简单的函数式编程，流行的`OOP面向对象编程`、还有c++的`GP泛型编程`，也就是模板编程。语言没有谁好谁坏，只有其适用场景，编程范式亦是如此。`c with class`我认为恰恰是`c++`最精华之处。

所以event模块中将`IO、timer、idle`统一抽象成事件，方便放入事件队列中统一调度，也是一种OOP的思想，而http模块中也不是全是`class`，也有很多函数式，强行封装成类，反而显得别扭。

而模板编程的核心是使静态类型语言具有动态类型的泛化，`STL`就是泛型编程的典范，其提供的容器如`vector、list、deque、map、set`、算法如`max、min、sort、count、find、search、transform`，应该是每个c++ coder应该熟练掌握的，即使如此，它的源码可读性还是很低，所以没有一定的功底和必要性，不推荐烂用模板编程。