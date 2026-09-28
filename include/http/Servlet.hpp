#pragma once

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "http/Http.hpp"

namespace sylar
{

class Servlet
{
public:
    virtual ~Servlet() = default;
    virtual HttpResponse handle(const HttpRequest& request) = 0;
};

class FunctionServlet final : public Servlet
{
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    explicit FunctionServlet(Handler handler);
    HttpResponse handle(const HttpRequest& request) override;

private:
    Handler m_handler;
};

class ServletDispatch final : public Servlet
{
public:
    void addExact(std::string path, std::shared_ptr<Servlet> servlet);
    void
    addPrefix(std::string prefix, std::shared_ptr<Servlet> servlet);
    HttpResponse handle(const HttpRequest& request) override;

private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<Servlet>> m_exact;
    std::vector<std::pair<std::string, std::shared_ptr<Servlet>>>
        m_prefix;
};

} // namespace sylar
