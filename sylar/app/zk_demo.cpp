#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

#include "fiber/BlockingExecutor.hpp"
#include "fiber/IOManager.hpp"
#include "zk/ZkClient.hpp"

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2)
        {
            throw std::invalid_argument("usage: zk_demo HOST:PORT");
        }
        sylar::IOManager io;
        sylar::BlockingExecutor blocking(1);
        io.schedule(
            [&]
            {
                blocking.await(
                    io,
                    [&]
                    {
                        auto zk = sylar::ZkClient::connect(argv[1]);
                        zk.create("/sylar_demo", "parent");
                        const std::string binary("v\0x", 3);
                        zk.create("/sylar_demo/item", binary, true);
                        if (!zk.exists("/sylar_demo/item") ||
                            zk.get("/sylar_demo/item") != binary)
                        {
                            throw std::runtime_error("ZooKeeper get "
                                                     "failed");
                        }
                        zk.set("/sylar_demo/item", "updated", 0);
                        if (zk.get("/sylar_demo/item") != "updated")
                        {
                            throw std::runtime_error("ZooKeeper set "
                                                     "failed");
                        }
                        const auto names = zk.children("/sylar_demo");
                        if (std::find(
                                names.begin(),
                                names.end(),
                                "item"
                            ) == names.end())
                        {
                            throw std::runtime_error(
                                "ZooKeeper child "
                                "missing"
                            );
                        }
                        zk.erase("/sylar_demo/item");
                        zk.erase("/sylar_demo");
                        if (zk.exists("/sylar_demo"))
                        {
                            throw std::runtime_error(
                                "ZooKeeper erase "
                                "failed"
                            );
                        }
                    }
                );
            }
        );
        io.run();
        std::cout << "zk ok" << std::endl;
    }
    catch (const std::exception& error)
    {
        std::cerr << "zk_demo: " << error.what() << std::endl;
        return 1;
    }
}