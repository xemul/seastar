#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/coroutine.hh>

using namespace seastar;

future<> do_demo() {
    auto statement = co_await create_scheduling_group("statement", 1000);
    auto commitlog = co_await create_scheduling_group("commitlog", 500);
    auto streaming = co_await create_scheduling_group("streaming", 100);
    co_await statement.update_io_bandwidth(200<<20); // 200MB/s
    co_await commitlog.update_io_bandwidth(100<<20); // 100MB/s
    co_await streaming.update_io_bandwidth(50<<20);  // 50MB/s
}

int main(int ac, char** av) {
    app_template app;
    return app.run(ac, av, [] {
        return do_demo();
    });
}
