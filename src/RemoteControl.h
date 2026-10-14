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

#ifndef PlayerbotsRemoteControl_h__
#define PlayerbotsRemoteControl_h__

#include "Define.h"
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace Playerbots
{
/// Line based TCP control port for the random bots.
///
/// Off by default (Playerbots.Remote.Port = 0). It needs a token, the first line of a connection must be
/// "AUTH <token>", anything else closes the connection. It binds to 127.0.0.1 unless configured otherwise.
/// One client is served at a time. The network thread never touches game state: commands are queued
/// and executed by Update(), which runs in the world thread.
class RemoteControl
{
public:
    static RemoteControl* instance();

    void Start();
    void Stop();
    void Update();

private:
    struct Request
    {
        std::string Line;
        std::promise<std::string> Response;
    };

    struct Impl;

    void Serve();
    std::string Execute(std::string const& line);
    std::string Submit(std::string line);

    std::unique_ptr<Impl> _impl;
    std::thread _thread;
    std::atomic<bool> _stop = false;
    std::mutex _mutex;
    std::queue<std::shared_ptr<Request>> _queue;
    std::string _token;
};
}

#define sRemoteControl Playerbots::RemoteControl::instance()

#endif // PlayerbotsRemoteControl_h__
