// bbport co-op: the part of shadPS4's file table that the network library uses.
// The runtime keeps game files in its own table (runtime_file.c); this one only holds
// sockets, epolls and resolvers, which the game addresses through sceNet* calls only.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "common/types.h"

namespace Libraries::Net {
struct Socket;
struct Epoll;
struct Resolver;
} // namespace Libraries::Net

namespace Core::FileSys {

enum class FileType { Regular, Directory, Device, Socket, Epoll, Resolver, Equeue };

struct File {
    std::atomic_bool is_opened{};
    std::atomic<FileType> type{FileType::Regular};
    std::string m_guest_name;
    std::mutex m_mutex;
    std::shared_ptr<Libraries::Net::Socket> socket;     // only valid for type == Socket
    std::shared_ptr<Libraries::Net::Epoll> epoll;       // only valid for type == Epoll
    std::shared_ptr<Libraries::Net::Resolver> resolver; // only valid for type == Resolver
};

class HandleTable {
public:
    // Ids 0-2 stay unused, as stdin/stdout/stderr do on the console.
    HandleTable() : m_files(3, nullptr) {}

    int CreateHandle() {
        std::scoped_lock lock{m_mutex};
        auto* file = new File{};
        for (size_t index = 3; index < m_files.size(); ++index) {
            if (!m_files[index]) {
                m_files[index] = file;
                return int(index);
            }
        }
        m_files.push_back(file);
        return int(m_files.size() - 1);
    }
    void DeleteHandle(int d) {
        std::scoped_lock lock{m_mutex};
        if (d >= 3 && size_t(d) < m_files.size()) {
            delete m_files[d];
            m_files[d] = nullptr;
        }
    }
    File* GetFile(int d) {
        std::scoped_lock lock{m_mutex};
        return d >= 0 && size_t(d) < m_files.size() ? m_files[d] : nullptr;
    }
    File* GetSocket(int d) { return Typed(d, FileType::Socket); }
    File* GetEpoll(int d) { return Typed(d, FileType::Epoll); }
    File* GetResolver(int d) { return Typed(d, FileType::Resolver); }

private:
    File* Typed(int d, FileType type) {
        File* file = GetFile(d);
        return file && file->type == type ? file : nullptr;
    }

    std::vector<File*> m_files;
    std::mutex m_mutex;
};

} // namespace Core::FileSys
