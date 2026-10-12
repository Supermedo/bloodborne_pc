// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "remix_renderer.h"
#include "remix_protocol.h"
namespace BbRemix {
class Client {
public:
    Client(); ~Client();
    Client(const Client&)=delete; Client& operator=(const Client&)=delete;
    bool Start(const std::filesystem::path& host, const std::filesystem::path& runtime,
               const std::filesystem::path& log, const Wire::Initialize& info);
    bool Render(const Wire::Writer& scene);
    void Stop();
    bool Ready() const;
    Renderer::SharedOutput Output() const;
    const std::string& Error() const;
private:
    struct Impl; std::unique_ptr<Impl> impl;
};
} // namespace BbRemix
