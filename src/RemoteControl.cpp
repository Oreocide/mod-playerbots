/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "RemoteControl.h"
#include "Config.h"
#include "Log.h"
#include "PlayerbotMgr.h"
#include "PlayerbotsConfig.h"
#include "Optional.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/time.h>
#endif

namespace Playerbots
{
namespace
{
constexpr size_t MinTokenLength = 16;
constexpr size_t MaxLineLength = 512;
constexpr std::chrono::seconds AuthTimeout(15);
constexpr std::chrono::seconds IdleTimeout(300);
constexpr std::chrono::seconds CommandTimeout(10);

bool ConstantTimeEquals(std::string const& a, std::string const& b)
{
    unsigned char diff = a.size() != b.size();
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i % std::max<size_t>(b.size(), 1)]);
    return diff == 0;
}
}

struct RemoteControl::Impl
{
    boost::asio::io_context Io;
    boost::asio::ip::tcp::acceptor Acceptor{ Io };
};

RemoteControl* RemoteControl::instance()
{
    static RemoteControl instance;
    return &instance;
}

void RemoteControl::Start()
{
    if (_impl)
        return;

    uint16 port = sPlayerbotsConfig.RemotePort;
    if (!port)
        return;

    _token = sPlayerbotsConfig.RemoteToken;
    if (_token.size() < MinTokenLength)
    {
        TC_LOG_ERROR("server.playerbots", "Playerbots: remote control not started, Playerbots.Remote.Token must be at least {} characters", MinTokenLength);
        return;
    }

    auto impl = std::make_unique<Impl>();
    boost::system::error_code error;
    boost::asio::ip::address address = boost::asio::ip::make_address(sPlayerbotsConfig.RemoteBindIp, error);
    if (error)
    {
        TC_LOG_ERROR("server.playerbots", "Playerbots: remote control not started, invalid Playerbots.Remote.BindIp '{}'", sPlayerbotsConfig.RemoteBindIp);
        return;
    }

    boost::asio::ip::tcp::endpoint endpoint(address, port);
    impl->Acceptor.open(endpoint.protocol(), error);
    if (!error)
        impl->Acceptor.set_option(boost::asio::socket_base::reuse_address(true), error);
    if (!error)
        impl->Acceptor.bind(endpoint, error);
    if (!error)
        impl->Acceptor.listen(1, error);
    if (!error)
        impl->Acceptor.non_blocking(true, error);
    if (error)
    {
        TC_LOG_ERROR("server.playerbots", "Playerbots: remote control not started, cannot listen on {}:{} ({})", address.to_string(), port, error.message());
        return;
    }

    if (!address.is_loopback())
        TC_LOG_WARN("server.playerbots", "Playerbots: remote control listens on {}, it is not encrypted, prefer 127.0.0.1 and an SSH tunnel", address.to_string());

    _impl = std::move(impl);
    _stop = false;
    _thread = std::thread(&RemoteControl::Serve, this);
    TC_LOG_INFO("server.playerbots", "Playerbots: remote control listening on {}:{}", address.to_string(), port);
}

void RemoteControl::Stop()
{
    if (!_impl)
        return;

    _stop = true;
    if (_thread.joinable())
        _thread.join();

    _impl.reset();

    std::lock_guard lock(_mutex);
    while (!_queue.empty())
        _queue.pop();
}

void RemoteControl::Update()
{
    if (!_impl)
        return;

    std::queue<std::shared_ptr<Request>> pending;
    {
        std::lock_guard lock(_mutex);
        pending.swap(_queue);
    }

    while (!pending.empty())
    {
        std::shared_ptr<Request> request = std::move(pending.front());
        pending.pop();
        request->Response.set_value(Execute(request->Line));
    }
}

std::string RemoteControl::Submit(std::string line)
{
    auto request = std::make_shared<Request>();
    request->Line = std::move(line);
    std::future<std::string> response = request->Response.get_future();
    {
        std::lock_guard lock(_mutex);
        _queue.push(request);
    }

    auto deadline = std::chrono::steady_clock::now() + CommandTimeout;
    while (!_stop && std::chrono::steady_clock::now() < deadline)
        if (response.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready)
            return response.get();

    return "ERR timeout";
}

void RemoteControl::Serve()
{
    while (!_stop)
    {
        boost::asio::ip::tcp::socket socket(_impl->Io);
        boost::system::error_code error;
        _impl->Acceptor.accept(socket, error);
        if (error)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        socket.non_blocking(false, error);

#ifndef _WIN32
        timeval timeout{ 1, 0 };
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

        std::string remote = socket.remote_endpoint(error).address().to_string();
        auto send = [&socket](std::string_view text)
        {
            boost::system::error_code sendError;
            std::string line(text);
            line += "\n";
            boost::asio::write(socket, boost::asio::buffer(line), sendError);
            return !sendError;
        };

        bool authenticated = false;
        std::string buffer;
        auto connected = std::chrono::steady_clock::now();
        auto lastActivity = connected;
        bool open = true;

        while (open && !_stop)
        {
            auto now = std::chrono::steady_clock::now();
            if ((!authenticated && now - connected > AuthTimeout) || now - lastActivity > IdleTimeout)
                break;

            char data[256];
            size_t read = socket.read_some(boost::asio::buffer(data), error);
            if (error)
            {
                // Receive timeouts surface as would_block/try_again, anything else ends the connection
                if (error == boost::asio::error::would_block || error == boost::asio::error::try_again)
                    continue;
                break;
            }

            lastActivity = now;
            buffer.append(data, read);
            if (buffer.size() > MaxLineLength * 2)
                break;

            size_t end;
            while (open && (end = buffer.find('\n')) != std::string::npos)
            {
                std::string line = buffer.substr(0, end);
                buffer.erase(0, end + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();

                if (line.size() > MaxLineLength)
                {
                    open = false;
                    break;
                }

                if (!authenticated)
                {
                    if (line.starts_with("AUTH ") && ConstantTimeEquals(line.substr(5), _token))
                    {
                        authenticated = true;
                        TC_LOG_INFO("server.playerbots", "Playerbots: remote control client {} authenticated", remote);
                        open = send("OK authenticated, type help");
                    }
                    else
                    {
                        TC_LOG_WARN("server.playerbots", "Playerbots: remote control client {} failed to authenticate", remote);
                        send("ERR authentication required");
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        open = false;
                    }
                    continue;
                }

                if (line.empty())
                    continue;

                if (line == "quit" || line == "exit")
                {
                    send("OK bye");
                    open = false;
                    continue;
                }

                open = send(Submit(std::move(line)));
            }
        }

        socket.close(error);
    }
}

// World thread
std::string RemoteControl::Execute(std::string const& line)
{
    std::string command = line;
    strToLower(command);
    std::vector<std::string_view> args = Trinity::Tokenize(command, ' ', false);
    if (args.empty())
        return "ERR empty command";

    if (args[0] == "help")
        return "OK commands: status | count <n> | stop | quit";

    if (args[0] == "status")
        return Trinity::StringFormat("OK random bots online {} of {} wanted, random bots {}",
            sPlayerbotMgr->GetRandomBotCount(), sPlayerbotsConfig.RandomCount, sPlayerbotsConfig.RandomEnabled ? "enabled" : "disabled");

    if (args[0] == "count")
    {
        if (args.size() != 2)
            return "ERR usage: count <n>";

        Optional<uint32> count = Trinity::StringTo<uint32>(args[1]);
        if (!count || *count > 2000)
            return "ERR count must be 0..2000";

        // Runtime only, a config reload restores the configured value
        sPlayerbotsConfig.RandomCount = *count;
        if (sPlayerbotMgr->GetRandomBotCount() > *count)
            sPlayerbotMgr->LogoutRandomBots();
        return Trinity::StringFormat("OK keeping {} random bots online", *count);
    }

    if (args[0] == "stop")
    {
        sPlayerbotsConfig.RandomCount = 0;
        sPlayerbotMgr->LogoutRandomBots();
        return "OK logging out all random bots";
    }

    return "ERR unknown command, type help";
}
}
