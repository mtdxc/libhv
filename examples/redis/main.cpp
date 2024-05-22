#include  "RedisClient.h"
#include "EventLoop.h"

int main(int argc, char** argv) {
    hloop_t* loop = hloop_new(0);
    RedisClient conn(loop);
    conn.open("192.168.4.12", 6379);
    conn.set("aa", "bb", [&](reply& r) {
        printf("set return %s\n", r.str().c_str());
        conn.get("aa", [](reply& r) {
            printf("get return %s\n", r.str().c_str());
        });
    });
    hloop_run(loop);
    hloop_free(&loop);
}
