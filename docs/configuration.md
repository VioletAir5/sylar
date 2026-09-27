# 配置系统

本模块按 sylar 原配置系统的使用方式提供 `sylar::Config`、`ConfigVar<T>` 和 `LexicalCast`，适配了当前项目的 C++23 结构。实现位于 [Config.hpp](../include/config/Config.hpp) 和 [Config.cpp](../sylar/src/config/Config.cpp)。

## 依赖与构建

需要 yaml-cpp 开发包。在 Ubuntu 上可安装 `libyaml-cpp-dev` 后正常运行 CMake。当前开发环境未全局安装该包，构建时把依赖包解压到了项目同级目录 `../yaml-cpp-deps`，使用：

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="$PWD/../yaml-cpp-deps/usr"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

第三方源码和包均不放入本项目目录。

## 使用

```cpp
#include "config/Config.hpp"

auto port = sylar::Config::Lookup<int>(
    "server.port", 8080, "listen port");

auto listenerId = port->addListener(
    [](int oldValue, int newValue) {
        // 配置值已经更新；这里可以更新相关组件。
    });

sylar::Config::LoadFromYaml(YAML::Load(
    "server:\n  port: 9000\n"));

int currentPort = port->getValue(); // 9000
port->delListener(listenerId);
```

配置名忽略大小写，允许字母、数字、下划线及用点号分隔的层级，例如 `server.port`。同名同类型的注册返回原变量；同名异类型返回空指针。支持标量、`vector`、`list`、`set`、`unordered_set`、`map<string, T>` 和 `unordered_map<string, T>`，容器可以嵌套。自定义类型可特化 `LexicalCast<std::string, T>` 和 `LexicalCast<T, std::string>`；示例见 [test_config.cpp](../tests/test_config.cpp)。

`LoadFromConfDir(path, force)` 递归读取 `.yml` 和 `.yaml` 文件，按路径排序后合并，同名配置只应用最终值；目录文件变化或新变量注册后重新加载。加载失败会抛异常，不记录成功状态，因此修复文件后可以重试。删除配置文件不会把已注册变量重置为默认值。

变量读写及监听器注册可并发使用。值变化后，先更新值、释放锁，再调用监听器，因此监听器可以读取或更新配置。监听器异常会传给调用方；其余监听器不会继续执行。单次 YAML 加载可能依次更新多个变量，不保证跨变量事务。当前日志系统没有全局 Logger 管理器；引入 config/LogLevelCodec.hpp 后，可注册 ConfigVar<sylar::log::LogLevel>，再用监听器更新某个 Logger 的级别，见测试中的 `AppliesYamlLevelToCurrentLogger`。