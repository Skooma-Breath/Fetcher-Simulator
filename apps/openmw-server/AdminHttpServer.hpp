#ifndef OPENMW_SERVER_ADMINHTTPSERVER_HPP
#define OPENMW_SERVER_ADMINHTTPSERVER_HPP

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace mwmp
{

class AdminHttpServer
{
public:
    struct Response
    {
        int status = 200;
        std::string contentType = "application/json; charset=utf-8";
        std::string body;
    };

    using Handler = std::function<Response(std::string_view action, const std::map<std::string, std::string>& query)>;

    explicit AdminHttpServer(Handler handler);
    ~AdminHttpServer();

    AdminHttpServer(const AdminHttpServer&) = delete;
    AdminHttpServer& operator=(const AdminHttpServer&) = delete;

    bool start(const std::string& host, int port, std::string* error = nullptr);
    void stop();

    bool isRunning() const;
    std::string url() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace mwmp

#endif
