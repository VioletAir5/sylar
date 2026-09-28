#include "http/Servlet.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace sylar
{

FunctionServlet::FunctionServlet(Handler handler)
    : m_handler(std::move(handler))
{
    if (!m_handler)
    {
        throw std::invalid_argument("empty servlet handler");
    }
}

HttpResponse FunctionServlet::handle(const HttpRequest& request)
{
    return m_handler(request);
}

void ServletDispatch::addExact(
    std::string path, std::shared_ptr<Servlet> servlet
)
{
    if (path.empty() || path.front() != '/' || !servlet)
    {
        throw std::invalid_argument("invalid exact route");
    }
    std::unique_lock lock(m_mutex);
    m_exact[std::move(path)] = std::move(servlet);
}

void ServletDispatch::addPrefix(
    std::string prefix, std::shared_ptr<Servlet> servlet
)
{
    if (prefix.empty() || prefix.front() != '/' || !servlet)
    {
        throw std::invalid_argument("invalid prefix route");
    }
    std::unique_lock lock(m_mutex);
    auto found = std::find_if(
        m_prefix.begin(),
        m_prefix.end(),
        [&](const auto& route) { return route.first == prefix; }
    );
    if (found == m_prefix.end())
    {
        m_prefix.emplace_back(std::move(prefix), std::move(servlet));
    }
    else
    {
        found->second = std::move(servlet);
    }
    std::sort(
        m_prefix.begin(),
        m_prefix.end(),
        [](const auto& a, const auto& b)
        { return a.first.size() > b.first.size(); }
    );
}

HttpResponse ServletDispatch::handle(const HttpRequest& request)
{
    const auto path = request.target.substr(
        0,
        request.target.find('?')
    );
    std::shared_ptr<Servlet> servlet;
    {
        std::shared_lock lock(m_mutex);
        if (const auto found = m_exact.find(path);
            found != m_exact.end())
        {
            servlet = found->second;
        }
        else
        {
            for (const auto& [prefix, route] : m_prefix)
            {
                if (path.starts_with(prefix))
                {
                    servlet = route;
                    break;
                }
            }
        }
    }
    if (servlet)
    {
        return servlet->handle(request);
    }
    HttpResponse response;
    response.status = 404;
    response.reason = "Not Found";
    response.headers["Content-Type"] = "text/plain; charset=utf-8";
    response.body = "404 Not Found\n";
    return response;
}

} // namespace sylar
