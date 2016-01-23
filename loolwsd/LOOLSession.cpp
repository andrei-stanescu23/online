/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <ftw.h>
#include <utime.h>

#include <cassert>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKit.h>
#include <LibreOfficeKit/LibreOfficeKitEnums.h>

#include <Poco/Dynamic/Var.h>
#include <Poco/Exception.h>
#include <Poco/File.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Net/HTTPStreamFactory.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/Path.h>
#include <Poco/Process.h>
#include <Poco/Random.h>
#include <Poco/StreamCopier.h>
#include <Poco/String.h>
#include <Poco/StringTokenizer.h>
#include <Poco/ThreadLocal.h>
#include <Poco/URI.h>
#include <Poco/URIStreamOpener.h>
#include <Poco/Util/Application.h>
#include <Poco/Exception.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/DialogSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/FileStream.h>

#include "LOKitHelper.hpp"
#include "LOOLProtocol.hpp"
#include "LOOLSession.hpp"
#include "LOOLWSD.hpp"
#include "TileCache.hpp"
#include "Util.hpp"
#include "Rectangle.hpp"

using namespace LOOLProtocol;

using Poco::Dynamic::Var;
using Poco::File;
using Poco::IOException;
using Poco::JSON::Object;
using Poco::JSON::Parser;
using Poco::Net::HTTPStreamFactory;
using Poco::Net::WebSocket;
using Poco::Path;
using Poco::Process;
using Poco::ProcessHandle;
using Poco::Random;
using Poco::StreamCopier;
using Poco::StringTokenizer;
using Poco::Thread;
using Poco::ThreadLocal;
using Poco::UInt64;
using Poco::URI;
using Poco::URIStreamOpener;
using Poco::Util::Application;
using Poco::Exception;
using Poco::Net::DialogSocket;
using Poco::Net::SocketAddress;
using Poco::Net::WebSocketException;

const std::string LOOLSession::jailDocumentURL = "/user/thedocument";

LOOLSession::LOOLSession(std::shared_ptr<WebSocket> ws, Kind kind) :
    _kind(kind),
    _ws(ws),
    _docURL("")
{
    std::cout << Util::logPrefix() << "LOOLSession ctor this=" << this << " " << _kind << " ws=" << _ws.get() << std::endl;
    if (kind == Kind::ToClient)
    {
        _kindString = "ToClient";
    }
    else if (kind == Kind::ToMaster)
    {
        _kindString = "ToMaster";
    }
    else if (kind == Kind::ToPrisoner)
    {
        _kindString = "ToPrisoner";
    }
}

LOOLSession::~LOOLSession()
{
    std::cout << Util::logPrefix() << "LOOLSession dtor this=" << this << " " << _kind << std::endl;
    if (_ws)
        Util::shutdownWebSocket(*_ws);
}

void LOOLSession::sendTextFrame(const std::string& text)
{
    std::unique_lock<std::mutex> lock(_mutex);

    _ws->sendFrame(text.data(), text.size());
}

void LOOLSession::sendBinaryFrame(const char *buffer, int length)
{
    std::unique_lock<std::mutex> lock(_mutex);

    if (length > 1000)
    {
        std::string nextmessage = "nextmessage: size=" + std::to_string(length);
        if (_ws)
            _ws->sendFrame(nextmessage.data(), nextmessage.size());
    }

    if (_ws)
        _ws->sendFrame(buffer, length, WebSocket::FRAME_BINARY);
}

void LOOLSession::parseDocOptions(const StringTokenizer& tokens, int& part, std::string& timestamp)
{
    // First token is the "load" command itself.
    size_t offset = 1;
    if (tokens.count() > 2 && tokens[1].find("part=") == 0)
    {
        getTokenInteger(tokens[1], "part", part);
        ++offset;
    }

    for (size_t i = offset; i < tokens.count(); ++i)
    {
        if (tokens[i].find("url=") == 0)
        {
            _docURL = tokens[i].substr(strlen("url="));
            ++offset;
        }
        else if (tokens[i].find("timestamp=") == 0)
        {
            timestamp = tokens[i].substr(strlen("timestamp="));
            ++offset;
        }
    }

    if (tokens.count() > offset)
    {
        if (getTokenString(tokens[offset], "options", _docOptions))
        {
            if (tokens.count() > offset + 1)
                _docOptions += Poco::cat(std::string(" "), tokens.begin() + offset + 1, tokens.end());
        }
    }
}

std::map<Process::PID, UInt64> MasterProcessSession::_childProcesses;

std::set<std::shared_ptr<MasterProcessSession>> MasterProcessSession::_availableChildSessions;
std::mutex MasterProcessSession::_availableChildSessionMutex;
std::condition_variable MasterProcessSession::_availableChildSessionCV;
Random MasterProcessSession::_rng;
std::mutex MasterProcessSession::_rngMutex;

MasterProcessSession::MasterProcessSession(std::shared_ptr<WebSocket> ws, Kind kind) :
    LOOLSession(ws, kind),
    _childId(0),
    _pidChild(0),
    _curPart(0),
    _loadPart(-1)
{
    std::cout << Util::logPrefix() << "MasterProcessSession ctor this=" << this << " ws=" << _ws.get() << std::endl;
}

MasterProcessSession::~MasterProcessSession()
{
    std::cout << Util::logPrefix() << "MasterProcessSession dtor this=" << this << " _peer=" << _peer.lock().get() << std::endl;

    auto peer = _peer.lock();
    if (_kind == Kind::ToClient && peer)
    {
        Util::shutdownWebSocket(*(peer->_ws));
    }
    else
    if (_kind == Kind::ToPrisoner && peer)
    {
        Util::shutdownWebSocket(*(peer->_ws));
    }
}

bool MasterProcessSession::handleInput(const char *buffer, int length)
{
    Application::instance().logger().information(Util::logPrefix() + _kindString + ",Input," + getAbbreviatedMessage(buffer, length));

    std::string firstLine = getFirstLine(buffer, length);
    StringTokenizer tokens(firstLine, " ", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);

    if (haveSeparateProcess())
    {
        // Note that this handles both forwarding requests from the client to the child process, and
        // forwarding replies from the child process to the client. Or does it?

        // Snoop at some  messages and manipulate tile cache information as needed
        auto peer = _peer.lock();

        if (_kind == Kind::ToPrisoner)
        {
            if (tokens[0] == "curpart:" &&
                tokens.count() == 2 &&
                getTokenInteger(tokens[1], "part", _curPart))
            {
                return true;
            }

            if (tokens.count() == 2 && tokens[0] == "saveas:")
            {
                std::string url;
                if (!getTokenString(tokens[1], "url", url))
                    return true;

                if (peer)
                {
                    // Save as completed, inform the other (Kind::ToClient)
                    // MasterProcessSession about it.

                    const std::string filePrefix("file:///");
                    if (url.find(filePrefix) == 0)
                    {
                        // Rewrite file:// URLs, as they are visible to the outside world.
                        Path path(MasterProcessSession::getJailPath(_childId), url.substr(filePrefix.length()));
                        url = filePrefix + path.toString().substr(1);
                    }
                    peer->_saveAsQueue.put(url);
                }

                return true;
            }
        }

        if (_kind == Kind::ToPrisoner && peer && peer->_tileCache)
        {
            if (tokens[0] == "tile:")
            {
                int part, width, height, tilePosX, tilePosY, tileWidth, tileHeight;
                if (tokens.count() < 8 ||
                    !getTokenInteger(tokens[1], "part", part) ||
                    !getTokenInteger(tokens[2], "width", width) ||
                    !getTokenInteger(tokens[3], "height", height) ||
                    !getTokenInteger(tokens[4], "tileposx", tilePosX) ||
                    !getTokenInteger(tokens[5], "tileposy", tilePosY) ||
                    !getTokenInteger(tokens[6], "tilewidth", tileWidth) ||
                    !getTokenInteger(tokens[7], "tileheight", tileHeight))
                    assert(false);

                assert(firstLine.size() < static_cast<std::string::size_type>(length));
                peer->_tileCache->saveTile(part, width, height, tilePosX, tilePosY, tileWidth, tileHeight, buffer + firstLine.size() + 1, length - firstLine.size() - 1);
            }
            else if (tokens[0] == "status:")
            {
                peer->_tileCache->saveTextFile(std::string(buffer, length), "status.txt");
            }
            else if (tokens[0] == "commandvalues:")
            {
                std::string stringMsg(buffer, length);
                std::string stringJSON = stringMsg.substr(stringMsg.find_first_of("{"));
                Parser parser;
                Var result = parser.parse(stringJSON);
                Object::Ptr object = result.extract<Object::Ptr>();
                std::string commandName = object->get("commandName").toString();
                if (commandName.find(".uno:CharFontName") != std::string::npos ||
                    commandName.find(".uno:StyleApply") != std::string::npos)
                {
                    // other commands should not be cached
                    peer->_tileCache->saveTextFile(std::string(buffer, length), "cmdValues" + commandName + ".txt");
                }
            }
            else if (tokens[0] == "partpagerectangles:")
            {
                if (tokens.count() > 1 && !tokens[1].empty())
                    peer->_tileCache->saveTextFile(std::string(buffer, length), "partpagerectangles.txt");
            }
            else if (tokens[0] == "invalidatecursor:")
            {
                peer->_tileCache->setEditing(true);
            }
            else if (tokens[0] == "invalidatetiles:")
            {
                // FIXME temporarily, set the editing on the 1st invalidate, TODO extend
                // the protocol so that the client can set the editing or view only.
                peer->_tileCache->setEditing(true);

                assert(firstLine.size() == static_cast<std::string::size_type>(length));
                peer->_tileCache->invalidateTiles(firstLine);
            }
            else if (tokens[0] == "renderfont:")
            {
                std::string font;
                if (tokens.count() < 2 ||
                    !getTokenString(tokens[1], "font", font))
                    assert(false);

                assert(firstLine.size() < static_cast<std::string::size_type>(length));
                peer->_tileCache->saveRendering(font, "font", buffer + firstLine.size() + 1, length - firstLine.size() - 1);
            }
        }

        forwardToPeer(buffer, length);
        return true;
    }

    if (tokens[0] == "child")
    {
        if (_kind != Kind::ToPrisoner)
        {
            sendTextFrame("error: cmd=child kind=invalid");
            return false;
        }
        if (!_peer.expired())
        {
            sendTextFrame("error: cmd=child kind=invalid");
            return false;
        }
        if (tokens.count() != 3)
        {
            sendTextFrame("error: cmd=child kind=syntax");
            return false;
        }

        UInt64 childId = std::stoull(tokens[1]);
        Process::PID pidChild = std::stoull(tokens[2]);

        std::unique_lock<std::mutex> lock(_availableChildSessionMutex);
        _availableChildSessions.insert(shared_from_this());
        std::cout << Util::logPrefix() << "Inserted " << this << " id=" << childId << " into _availableChildSessions, size=" << _availableChildSessions.size() << std::endl;
        _childId = childId;
        _pidChild = pidChild;
        lock.unlock();
        _availableChildSessionCV.notify_one();

        // log first lokit child pid information
        if ( LOOLWSD::doTest )
        {
            Poco::FileOutputStream filePID(LOOLWSD::LOKIT_PIDLOG);
            if (filePID.good())
                filePID << pidChild;
        }
    }
    else if (_kind == Kind::ToPrisoner)
    {
        // Message from child process to be forwarded to client.

        // I think we should never get here
        assert(false);
    }
    else if (tokens[0] == "load")
    {
        if (_docURL != "")
        {
            sendTextFrame("error: cmd=load kind=docalreadyloaded");
            return false;
        }
        return loadDocument(buffer, length, tokens);
    }
    else if (tokens[0] != "canceltiles" &&
             tokens[0] != "clientzoom" &&
             tokens[0] != "commandvalues" &&
             tokens[0] != "downloadas" &&
             tokens[0] != "getchildid" &&
             tokens[0] != "gettextselection" &&
             tokens[0] != "paste" &&
             tokens[0] != "insertfile" &&
             tokens[0] != "invalidatetiles" &&
             tokens[0] != "key" &&
             tokens[0] != "mouse" &&
             tokens[0] != "partpagerectangles" &&
             tokens[0] != "renderfont" &&
             tokens[0] != "requestloksession" &&
             tokens[0] != "resetselection" &&
             tokens[0] != "saveas" &&
             tokens[0] != "selectgraphic" &&
             tokens[0] != "selecttext" &&
             tokens[0] != "setclientpart" &&
             tokens[0] != "setpage" &&
             tokens[0] != "status" &&
             tokens[0] != "tile" &&
             tokens[0] != "tilecombine" &&
             tokens[0] != "uno")
    {
        sendTextFrame("error: cmd=" + tokens[0] + " kind=unknown");
        return false;
    }
    else if (_docURL == "")
    {
        sendTextFrame("error: cmd=" + tokens[0] + " kind=nodocloaded");
        return false;
    }
    else if (tokens[0] == "canceltiles")
    {
        if (!_peer.expired())
            forwardToPeer(buffer, length);
    }
    else if (tokens[0] == "commandvalues")
    {
        return getCommandValues(buffer, length, tokens);
    }
    else if (tokens[0] == "partpagerectangles")
    {
        return getPartPageRectangles(buffer, length);
    }
    else if (tokens[0] == "invalidatetiles")
    {
        return invalidateTiles(buffer, length, tokens);
    }
    else if (tokens[0] == "renderfont")
    {
        sendFontRendering(buffer, length, tokens);
    }
    else if (tokens[0] == "status")
    {
        return getStatus(buffer, length);
    }
    else if (tokens[0] == "tile")
    {
        sendTile(buffer, length, tokens);
    }
    else if (tokens[0] == "tilecombine")
    {
        sendCombinedTiles(buffer, length, tokens);
    }
    else
    {
        // All other commands are such that they always require a
        // LibreOfficeKitDocument session, i.e. need to be handled in
        // a child process.

        if (_peer.expired())
            dispatchChild();
        if (tokens[0] != "requestloksession")
        {
            forwardToPeer(buffer, length);
        }

        if ((tokens.count() > 1 && tokens[0] == "uno" && tokens[1] == ".uno:Save"))
        {
           _tileCache->documentSaved();
        }
    }
    return true;
}

bool MasterProcessSession::haveSeparateProcess()
{
    return _childId != 0;
}

Path MasterProcessSession::getJailPath(UInt64 childId)
{
    return Path::forDirectory(LOOLWSD::childRoot + Path::separator() + std::to_string(childId));
}

bool MasterProcessSession::invalidateTiles(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    int part, tilePosX, tilePosY, tileWidth, tileHeight;

    if (tokens.count() != 6 ||
        !getTokenInteger(tokens[1], "part", part) ||
        !getTokenInteger(tokens[2], "tileposx", tilePosX) ||
        !getTokenInteger(tokens[3], "tileposy", tilePosY) ||
        !getTokenInteger(tokens[4], "tilewidth", tileWidth) ||
        !getTokenInteger(tokens[5], "tileheight", tileHeight))
    {
        sendTextFrame("error: cmd=invalidatetiles kind=syntax");
        return false;
    }

    // FIXME temporarily, set the editing on the 1st invalidate, TODO extend
    // the protocol so that the client can set the editing or view only.
    _tileCache->setEditing(true);

    _tileCache->invalidateTiles(_curPart, tilePosX, tilePosY, tileWidth, tileHeight);
    return true;
}

bool MasterProcessSession::loadDocument(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    if (tokens.count() < 2)
    {
        sendTextFrame("error: cmd=load kind=syntax");
        return false;
    }

    std::string timestamp;
    parseDocOptions(tokens, _loadPart, timestamp);

    try
    {
        URI aUri(_docURL);
    }
    catch(Poco::SyntaxException&)
    {
        sendTextFrame("error: cmd=load kind=uriinvalid");
        return false;
    }

    _tileCache.reset(new TileCache(_docURL, timestamp));

    return true;
}

bool MasterProcessSession::getStatus(const char *buffer, int length)
{
    std::string status;

    status = _tileCache->getTextFile("status.txt");
    if (status.size() > 0)
    {
        sendTextFrame(status);
        return true;
    }

    if (_peer.expired())
        dispatchChild();
    forwardToPeer(buffer, length);
    return true;
}

bool MasterProcessSession::getCommandValues(const char *buffer, int length, StringTokenizer& tokens)
{
    std::string command;
    if (tokens.count() != 2 || !getTokenString(tokens[1], "command", command))
    {
        sendTextFrame("error: cmd=commandvalues kind=syntax");
        return false;
    }

    std::string cmdValues = _tileCache->getTextFile("cmdValues" + command + ".txt");
    if (cmdValues.size() > 0)
    {
        sendTextFrame(cmdValues);
        return true;
    }

    if (_peer.expired())
        dispatchChild();
    forwardToPeer(buffer, length);
    return true;
}

bool MasterProcessSession::getPartPageRectangles(const char *buffer, int length)
{
    std::string partPageRectangles = _tileCache->getTextFile("partpagerectangles.txt");
    if (partPageRectangles.size() > 0)
    {
        sendTextFrame(partPageRectangles);
        return true;
    }

    if (_peer.expired())
        dispatchChild();
    forwardToPeer(buffer, length);
    return true;
}

std::string MasterProcessSession::getSaveAs()
{
    return _saveAsQueue.get();
}

void MasterProcessSession::sendFontRendering(const char *buffer, int length, StringTokenizer& tokens)
{
    std::string font;

    if (tokens.count() < 2 ||
        !getTokenString(tokens[1], "font", font))
    {
        sendTextFrame("error: cmd=renderfont kind=syntax");
        return;
    }

    std::string response = "renderfont: " + Poco::cat(std::string(" "), tokens.begin() + 1, tokens.end()) + "\n";

    std::vector<char> output;
    output.resize(response.size());
    std::memcpy(output.data(), response.data(), response.size());

    std::unique_ptr<std::fstream> cachedRendering = _tileCache->lookupRendering(font, "font");
    if (cachedRendering && cachedRendering->is_open())
    {
        cachedRendering->seekg(0, std::ios_base::end);
        size_t pos = output.size();
        std::streamsize size = cachedRendering->tellg();
        output.resize(pos + size);
        cachedRendering->seekg(0, std::ios_base::beg);
        cachedRendering->read(output.data() + pos, size);
        cachedRendering->close();

        sendBinaryFrame(output.data(), output.size());
        return;
    }

    if (_peer.expired())
        dispatchChild();
    forwardToPeer(buffer, length);
}

void MasterProcessSession::sendTile(const char *buffer, int length, StringTokenizer& tokens)
{
    int part, width, height, tilePosX, tilePosY, tileWidth, tileHeight;

    if (tokens.count() < 8 ||
        !getTokenInteger(tokens[1], "part", part) ||
        !getTokenInteger(tokens[2], "width", width) ||
        !getTokenInteger(tokens[3], "height", height) ||
        !getTokenInteger(tokens[4], "tileposx", tilePosX) ||
        !getTokenInteger(tokens[5], "tileposy", tilePosY) ||
        !getTokenInteger(tokens[6], "tilewidth", tileWidth) ||
        !getTokenInteger(tokens[7], "tileheight", tileHeight))
    {
        sendTextFrame("error: cmd=tile kind=syntax");
        return;
    }

    if (part < 0 ||
        width <= 0 ||
        height <= 0 ||
        tilePosX < 0 ||
        tilePosY < 0 ||
        tileWidth <= 0 ||
        tileHeight <= 0)
    {
        sendTextFrame("error: cmd=tile kind=invalid");
        return;
    }

    std::string response = "tile: " + Poco::cat(std::string(" "), tokens.begin() + 1, tokens.end()) + "\n";

    std::vector<char> output;
    output.reserve(4 * width * height);
    output.resize(response.size());
    std::memcpy(output.data(), response.data(), response.size());

    std::unique_ptr<std::fstream> cachedTile = _tileCache->lookupTile(part, width, height, tilePosX, tilePosY, tileWidth, tileHeight);
    if (cachedTile && cachedTile->is_open())
    {
        cachedTile->seekg(0, std::ios_base::end);
        size_t pos = output.size();
        std::streamsize size = cachedTile->tellg();
        output.resize(pos + size);
        cachedTile->seekg(0, std::ios_base::beg);
        cachedTile->read(output.data() + pos, size);
        cachedTile->close();

        sendBinaryFrame(output.data(), output.size());

        return;
    }

    if (_peer.expired())
        dispatchChild();
    forwardToPeer(buffer, length);
}

void MasterProcessSession::sendCombinedTiles(const char *buffer, int length, StringTokenizer& /*tokens*/)
{
    // This is for invalidation - we should not have cached tiles
    if (_peer.expired())
        dispatchChild();
    forwardToPeer(buffer, length);
}

void MasterProcessSession::dispatchChild()
{
    // Copy document into jail using the fixed name

    std::shared_ptr<MasterProcessSession> childSession;
    std::unique_lock<std::mutex> lock(_availableChildSessionMutex);

    std::cout << Util::logPrefix() << "_availableChildSessions size=" << _availableChildSessions.size() << std::endl;

    if (_availableChildSessions.size() == 0)
    {
        std::cout << Util::logPrefix() << "waiting for a child session to become available" << std::endl;
        if (_availableChildSessionCV.wait_for(lock, std::chrono::minutes(5), [] { return _availableChildSessions.size() > 0; }))
            std::cout << Util::logPrefix() << "waiting done" << std::endl;
        else
        {
            std::cout << Util::logPrefix() << "waiting for the child session timed out, last try to start a new session" << std::endl;
            LOOLWSD::_namedMutexLOOL.lock();
            ++reinterpret_cast<size_t*>(LOOLWSD::_sharedForkChild.begin())[0];
            LOOLWSD::_namedMutexLOOL.unlock();

            if (!_availableChildSessionCV.wait_for(lock, std::chrono::minutes(5), [] { return _availableChildSessions.size() > 0; }))
            {
                std::cout << Util::logPrefix() << "real trouble starting new session, giving up" << std::endl;
                lock.unlock();
                return;
            }
        }
    }

    childSession = *(_availableChildSessions.begin());
    _availableChildSessions.erase(childSession);
    lock.unlock();

    if (_availableChildSessions.size() == 0 && !LOOLWSD::doTest)
    {
        LOOLWSD::_namedMutexLOOL.lock();
        std::cout << Util::logPrefix() << "No available child sessions, queue new child session" << std::endl;
        ++reinterpret_cast<size_t*>(LOOLWSD::_sharedForkChild.begin())[0];
        LOOLWSD::_namedMutexLOOL.unlock();
    }

    // Assume a valid URI
    URI aUri(_docURL);

    if (aUri.isRelative())
        aUri = URI( URI("file://"), aUri.toString() );

    if (!aUri.empty() && aUri.getScheme() == "file")
    {
        std::string aJailDoc = jailDocumentURL.substr(1) + Path::separator() + std::to_string(childSession->_pidChild);
        Path aSrcFile(aUri.getPath());
        Path aDstFile(Path(getJailPath(childSession->_childId), aJailDoc), aSrcFile.getFileName());
        Path aDstPath(getJailPath(childSession->_childId), aJailDoc);
        Path aJailFile(aJailDoc, aSrcFile.getFileName());

        try
        {
            File(aDstPath).createDirectories();
        }
        catch (Exception& exc)
        {
            Application::instance().logger().error( Util::logPrefix() +
                "createDirectories(\"" + aDstPath.toString() + "\") failed: " + exc.displayText() );

        }

        // cleanup potential leftovers from the last time
        File aToCleanup(aDstFile);
        if (aToCleanup.exists())
            aToCleanup.remove();

#ifdef __linux
        Application::instance().logger().information(Util::logPrefix() + "Linking " + aSrcFile.toString() + " to " + aDstFile.toString());
        if (link(aSrcFile.toString().c_str(), aDstFile.toString().c_str()) == -1)
        {
            // Failed
            Application::instance().logger().error( Util::logPrefix() +
                "link(\"" + aSrcFile.toString() + "\",\"" + aDstFile.toString() + "\") failed: " + strerror(errno) );
        }
#endif

        try
        {
            //fallback
            if (!File(aDstFile).exists())
            {
                Application::instance().logger().information(Util::logPrefix() + "Copying " + aSrcFile.toString() + " to " + aDstFile.toString());
                File(aSrcFile).copyTo(aDstFile.toString());
            }
        }
        catch (Exception& exc)
        {
            Application::instance().logger().error( Util::logPrefix() +
                "copyTo(\"" + aSrcFile.toString() + "\",\"" + aDstFile.toString() + "\") failed: " + exc.displayText());
        }
    }

    _peer = childSession;
    childSession->_peer = shared_from_this();

    std::string loadRequest = "load" + (_loadPart >= 0 ?  " part=" + std::to_string(_loadPart) : "") + " url=" + _docURL + (!_docOptions.empty() ? " options=" + _docOptions : "");
    forwardToPeer(loadRequest.c_str(), loadRequest.size());
}

void MasterProcessSession::forwardToPeer(const char *buffer, int length)
{
    Application::instance().logger().information(Util::logPrefix() + _kindString + ",forwardToPeer," + getAbbreviatedMessage(buffer, length));
    auto peer = _peer.lock();
    if (!peer)
        return;
    peer->sendBinaryFrame(buffer, length);
}

ChildProcessSession::ChildProcessSession(std::shared_ptr<WebSocket> ws, LibreOfficeKit *loKit, std::string childId) :
    LOOLSession(ws, Kind::ToMaster),
    _loKitDocument(NULL),
    _loKit(loKit),
    _childId(childId),
    _clientPart(0)
{
    std::cout << Util::logPrefix() << "ChildProcessSession ctor this=" << this << " ws=" << _ws.get() << std::endl;
}

ChildProcessSession::~ChildProcessSession()
{
    std::cout << Util::logPrefix() << "ChildProcessSession dtor this=" << this << std::endl;
    if (LIBREOFFICEKIT_HAS(_loKit, registerCallback))
        _loKit->pClass->registerCallback(_loKit, 0, 0);
}

bool ChildProcessSession::handleInput(const char *buffer, int length)
{
    std::string firstLine = getFirstLine(buffer, length);
    StringTokenizer tokens(firstLine, " ", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);

    Application::instance().logger().information(Util::logPrefix() + _kindString + ",Input," + getAbbreviatedMessage(buffer, length));

    if (tokens[0] == "canceltiles")
    {
        // this command makes sense only on the command queue level, nothing
        // to do here
        return true;
    }
    else if (tokens[0] == "commandvalues")
    {
        return getCommandValues(buffer, length, tokens);
    }
    else if (tokens[0] == "partpagerectangles")
    {
        return getPartPageRectangles(buffer, length);
    }
    else if (tokens[0] == "load")
    {
        if (_docURL != "")
        {
            sendTextFrame("error: cmd=load kind=docalreadyloaded");
            return false;
        }
        return loadDocument(buffer, length, tokens);
    }
    else if (_docURL == "")
    {
        sendTextFrame("error: cmd=" + tokens[0] + " kind=nodocloaded");
        return false;
    }
    else if (tokens[0] == "renderfont")
    {
        sendFontRendering(buffer, length, tokens);
    }
    else if (tokens[0] == "setclientpart")
    {
        return setClientPart(buffer, length, tokens);
    }
    else if (tokens[0] == "setpage")
    {
        return setPage(buffer, length, tokens);
    }
    else if (tokens[0] == "status")
    {
        return getStatus(buffer, length);
    }
    else if (tokens[0] == "tile")
    {
        sendTile(buffer, length, tokens);
    }
    else if (tokens[0] == "tilecombine")
    {
        sendCombinedTiles(buffer, length, tokens);
    }
    else
    {
        // All other commands are such that they always require a LibreOfficeKitDocument session,
        // i.e. need to be handled in a child process.

        assert(tokens[0] == "clientzoom" ||
               tokens[0] == "downloadas" ||
               tokens[0] == "getchildid" ||
               tokens[0] == "gettextselection" ||
               tokens[0] == "paste" ||
               tokens[0] == "insertfile" ||
               tokens[0] == "key" ||
               tokens[0] == "mouse" ||
               tokens[0] == "uno" ||
               tokens[0] == "selecttext" ||
               tokens[0] == "selectgraphic" ||
               tokens[0] == "resetselection" ||
               tokens[0] == "saveas");

        if (_docType != "text" && _loKitDocument->pClass->getPart(_loKitDocument) != _clientPart)
        {
            _loKitDocument->pClass->setPart(_loKitDocument, _clientPart);
        }
        if (tokens[0] == "clientzoom")
        {
            return clientZoom(buffer, length, tokens);
        }
        else if (tokens[0] == "downloadas")
        {
            return downloadAs(buffer, length, tokens);
        }
        else if (tokens[0] == "getchildid")
        {
            return getChildId();
        }
        else if (tokens[0] == "gettextselection")
        {
            return getTextSelection(buffer, length, tokens);
        }
        else if (tokens[0] == "paste")
        {
            return paste(buffer, length, tokens);
        }
        else if (tokens[0] == "insertfile")
        {
            return insertFile(buffer, length, tokens);
        }
        else if (tokens[0] == "key")
        {
            return keyEvent(buffer, length, tokens);
        }
        else if (tokens[0] == "mouse")
        {
            return mouseEvent(buffer, length, tokens);
        }
        else if (tokens[0] == "uno")
        {
            return unoCommand(buffer, length, tokens);
        }
        else if (tokens[0] == "selecttext")
        {
            return selectText(buffer, length, tokens);
        }
        else if (tokens[0] == "selectgraphic")
        {
            return selectGraphic(buffer, length, tokens);
        }
        else if (tokens[0] == "resetselection")
        {
            return resetSelection(buffer, length, tokens);
        }
        else if (tokens[0] == "saveas")
        {
            return saveAs(buffer, length, tokens);
        }
        else
        {
            assert(false);
        }
    }
    return true;
}

extern "C"
{
    static void myCallback(int nType, const char* pPayload, void* pData)
    {
        ChildProcessSession *srv = reinterpret_cast<ChildProcessSession *>(pData);

        switch ((LibreOfficeKitCallbackType) nType)
        {
        case LOK_CALLBACK_INVALIDATE_TILES:
            {
                int curPart = srv->_loKitDocument->pClass->getPart(srv->_loKitDocument);
                srv->sendTextFrame("curpart: part=" + std::to_string(curPart));
                if (srv->_docType == "text")
                {
                    curPart = 0;
                }
                StringTokenizer tokens(std::string(pPayload), " ", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);
                if (tokens.count() == 4)
                {
                    int x, y, width, height;

                    try {
                        x = std::stoi(tokens[0]);
                        y = std::stoi(tokens[1]);
                        width = std::stoi(tokens[2]);
                        height = std::stoi(tokens[3]);
                    }
                    catch (std::out_of_range&)
                    {
                        // something went wrong, invalidate everything
                        Application::instance().logger().information(Util::logPrefix() + "Ignoring integer values out of range: " + pPayload);
                        x = 0;
                        y = 0;
                        width = INT_MAX;
                        height = INT_MAX;
                    }

                    srv->sendTextFrame("invalidatetiles:"
                                       " part=" + std::to_string(curPart) +
                                       " x=" + std::to_string(x) +
                                       " y=" + std::to_string(y) +
                                       " width=" + std::to_string(width) +
                                       " height=" + std::to_string(height));
                }
                else
                {
                    srv->sendTextFrame("invalidatetiles: " + std::string(pPayload));
                }
            }
            break;
        case LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR:
            srv->sendTextFrame("invalidatecursor: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_TEXT_SELECTION:
            srv->sendTextFrame("textselection: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_TEXT_SELECTION_START:
            srv->sendTextFrame("textselectionstart: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_TEXT_SELECTION_END:
            srv->sendTextFrame("textselectionend: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_CURSOR_VISIBLE:
            srv->sendTextFrame("cursorvisible: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_GRAPHIC_SELECTION:
            srv->sendTextFrame("graphicselection: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_CELL_CURSOR:
            srv->sendTextFrame("cellcursor: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_CELL_FORMULA:
            srv->sendTextFrame("cellformula: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_MOUSE_POINTER:
            srv->sendTextFrame("mousepointer: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_HYPERLINK_CLICKED:
            srv->sendTextFrame("hyperlinkclicked: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_STATE_CHANGED:
            srv->sendTextFrame("statechanged: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_STATUS_INDICATOR_START:
            srv->sendTextFrame("statusindicatorstart:");
            break;
        case LOK_CALLBACK_STATUS_INDICATOR_SET_VALUE:
            srv->sendTextFrame("statusindicatorsetvalue: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_STATUS_INDICATOR_FINISH:
            srv->sendTextFrame("statusindicatorfinish:");
            break;
        case LOK_CALLBACK_SEARCH_NOT_FOUND:
            srv->sendTextFrame("searchnotfound: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_SEARCH_RESULT_SELECTION:
            srv->sendTextFrame("searchresultselection: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_DOCUMENT_SIZE_CHANGED:
            srv->getStatus("", 0);
            srv->getPartPageRectangles("", 0);
            break;
        case LOK_CALLBACK_SET_PART:
            srv->sendTextFrame("setpart: " + std::string(pPayload));
            break;
        case LOK_CALLBACK_UNO_COMMAND_RESULT:
            srv->sendTextFrame("unocommandresult: " + std::string(pPayload));
            break;
        }
    }
}

bool ChildProcessSession::loadDocument(const char *buffer, int length, StringTokenizer& tokens)
{
    int part = -1;
    if (tokens.count() < 2)
    {
        sendTextFrame("error: cmd=load kind=syntax");
        return false;
    }

    std::string timestamp;
    parseDocOptions(tokens, part, timestamp);

    URI aUri;
    try
    {
        aUri = URI(_docURL);
    }
    catch(Poco::SyntaxException&)
    {
        sendTextFrame("error: cmd=load kind=uriinvalid");
        return false;
    }

    if (aUri.empty())
    {
        sendTextFrame("error: cmd=load kind=uriempty");
        return false;
    }

    // The URL in the request is the original one, not visible in the chroot jail.
    // The child process uses the fixed name jailDocumentURL.

    if (LIBREOFFICEKIT_HAS(_loKit, registerCallback))
        _loKit->pClass->registerCallback(_loKit, myCallback, this);

    if (aUri.isRelative() || aUri.getScheme() == "file")
        aUri = URI( URI("file://"), Path(jailDocumentURL + Path::separator() + std::to_string(Process::id()),
                    Path(aUri.getPath()).getFileName()).toString() );

    if ((_loKitDocument = _loKit->pClass->documentLoad(_loKit, aUri.toString().c_str())) == NULL)
    {
        sendTextFrame("error: cmd=load kind=failed");
        Application::instance().logger().information(Util::logPrefix() + "Failed to load: " + aUri.toString() + ", error is: " + _loKit->pClass->getError(_loKit));
        return false;
    }

    std::string renderingOptions;
    if (!_docOptions.empty())
    {
        Poco::JSON::Parser parser;
        Poco::Dynamic::Var var = parser.parse(_docOptions);
        Poco::JSON::Object::Ptr object = var.extract<Poco::JSON::Object::Ptr>();
        renderingOptions = object->get("rendering").toString();
    }

    _loKitDocument->pClass->initializeForRendering(_loKitDocument, (renderingOptions.empty() ? nullptr : renderingOptions.c_str()));

    if ( _docType != "text" && part != -1)
    {
        _clientPart = part;
        _loKitDocument->pClass->setPart(_loKitDocument, part);
    }

    if (!getStatus(buffer, length))
        return false;

    _loKitDocument->pClass->registerCallback(_loKitDocument, myCallback, this);

    return true;
}

void ChildProcessSession::sendFontRendering(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    std::string font, decodedFont;
    int width, height;
    unsigned char *pixmap;

    if (tokens.count() < 2 ||
        !getTokenString(tokens[1], "font", font))
    {
        sendTextFrame("error: cmd=renderfont kind=syntax");
        return;
    }

    URI::decode(font, decodedFont);
    std::string response = "renderfont: " + Poco::cat(std::string(" "), tokens.begin() + 1, tokens.end()) + "\n";

    std::vector<char> output;
    output.resize(response.size());
    std::memcpy(output.data(), response.data(), response.size());

    Poco::Timestamp timestamp;
    pixmap = _loKitDocument->pClass->renderFont(_loKitDocument, decodedFont.c_str(), &width, &height);
    std::cout << Util::logPrefix() << "renderFont called, font[" << font << "] rendered in " << double(timestamp.elapsed())/1000 <<  "ms" << std::endl;

    if (pixmap != nullptr) {
        if (!Util::encodeBufferToPNG(pixmap, width, height, output, LOK_TILEMODE_RGBA))
        {
            sendTextFrame("error: cmd=renderfont kind=failure");
            delete[] pixmap;
            return;
        }
        delete[] pixmap;
    }

    sendBinaryFrame(output.data(), output.size());
}

bool ChildProcessSession::getStatus(const char* /*buffer*/, int /*length*/)
{
    std::string status = "status: " + LOKitHelper::documentStatus(_loKitDocument);
    StringTokenizer tokens(status, " ", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);
    if (!getTokenString(tokens[1], "type", _docType))
    {
        Application::instance().logger().information(Util::logPrefix() + "failed to get document type from" + status);
    }
    sendTextFrame(status);

    return true;
}

bool ChildProcessSession::getCommandValues(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    std::string command;
    if (tokens.count() != 2 || !getTokenString(tokens[1], "command", command))
    {
        sendTextFrame("error: cmd=commandvalues kind=syntax");
        return false;
    }
    sendTextFrame("commandvalues: " + std::string(_loKitDocument->pClass->getCommandValues(_loKitDocument, command.c_str())));
    return true;
}

bool ChildProcessSession::getPartPageRectangles(const char* /*buffer*/, int /*length*/)
{
    sendTextFrame("partpagerectangles: " + std::string(_loKitDocument->pClass->getPartPageRectangles(_loKitDocument)));
    return true;
}

void ChildProcessSession::sendTile(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    int part, width, height, tilePosX, tilePosY, tileWidth, tileHeight;

    if (tokens.count() < 8 ||
        !getTokenInteger(tokens[1], "part", part) ||
        !getTokenInteger(tokens[2], "width", width) ||
        !getTokenInteger(tokens[3], "height", height) ||
        !getTokenInteger(tokens[4], "tileposx", tilePosX) ||
        !getTokenInteger(tokens[5], "tileposy", tilePosY) ||
        !getTokenInteger(tokens[6], "tilewidth", tileWidth) ||
        !getTokenInteger(tokens[7], "tileheight", tileHeight))
    {
        sendTextFrame("error: cmd=tile kind=syntax");
        return;
    }

    if (part < 0 ||
        width <= 0 ||
        height <= 0 ||
        tilePosX < 0 ||
        tilePosY < 0 ||
        tileWidth <= 0 ||
        tileHeight <= 0)
    {
        sendTextFrame("error: cmd=tile kind=invalid");
        return;
    }

    std::string response = "tile: " + Poco::cat(std::string(" "), tokens.begin() + 1, tokens.end()) + "\n";

    std::vector<char> output;
    output.reserve(4 * width * height);
    output.resize(response.size());
    std::memcpy(output.data(), response.data(), response.size());

    unsigned char *pixmap = new unsigned char[4 * width * height];
    memset(pixmap, 0, 4 * width * height);

    if (_docType != "text" && part != _loKitDocument->pClass->getPart(_loKitDocument))
    {
        _loKitDocument->pClass->setPart(_loKitDocument, part);
    }

    Poco::Timestamp timestamp;
    _loKitDocument->pClass->paintTile(_loKitDocument, pixmap, width, height, tilePosX, tilePosY, tileWidth, tileHeight);
    std::cout << Util::logPrefix() << "paintTile called, tile at [" << tilePosX << ", " << tilePosY << "] rendered in " << double(timestamp.elapsed())/1000 <<  "ms" << std::endl;

    LibreOfficeKitTileMode mode = static_cast<LibreOfficeKitTileMode>(_loKitDocument->pClass->getTileMode(_loKitDocument));
    if (!Util::encodeBufferToPNG(pixmap, width, height, output, mode))
    {
        sendTextFrame("error: cmd=tile kind=failure");
        return;
    }

    delete[] pixmap;

    sendBinaryFrame(output.data(), output.size());
}

void ChildProcessSession::sendCombinedTiles(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    int part, pixelWidth, pixelHeight, tileWidth, tileHeight;
    std::string tilePositionsX, tilePositionsY;

    if (tokens.count() < 8 ||
        !getTokenInteger(tokens[1], "part", part) ||
        !getTokenInteger(tokens[2], "width", pixelWidth) ||
        !getTokenInteger(tokens[3], "height", pixelHeight) ||
        !getTokenString (tokens[4], "tileposx", tilePositionsX) ||
        !getTokenString (tokens[5], "tileposy", tilePositionsY) ||
        !getTokenInteger(tokens[6], "tilewidth", tileWidth) ||
        !getTokenInteger(tokens[7], "tileheight", tileHeight))
    {
        sendTextFrame("error: cmd=tilecombine kind=syntax");
        return;
    }

    if (part < 0 || pixelWidth <= 0 || pixelHeight <= 0
       || tileWidth <= 0 || tileHeight <= 0
       || tilePositionsX.empty() || tilePositionsY.empty())
    {
        sendTextFrame("error: cmd=tilecombine kind=invalid");
        return;
    }

    Util::Rectangle renderArea;

    StringTokenizer positionXtokens(tilePositionsX, ",", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);
    StringTokenizer positionYtokens(tilePositionsY, ",", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);

    size_t numberOfPositions = positionYtokens.count();

    // check that number of positions for X and Y is the same
    if (numberOfPositions != positionYtokens.count())
    {
        sendTextFrame("error: cmd=tilecombine kind=invalid");
        return;
    }

    std::vector<Util::Rectangle> tiles;
    tiles.reserve(numberOfPositions);

    for (size_t i = 0; i < numberOfPositions; i++)
    {
        int x, y;

        if (!stringToInteger(positionXtokens[i], x))
        {
            sendTextFrame("error: cmd=tilecombine kind=syntax");
            return;
        }
        if (!stringToInteger(positionYtokens[i], y))
        {
            sendTextFrame("error: cmd=tilecombine kind=syntax");
            return;
        }

        Util::Rectangle rectangle(x, y, tileWidth, tileHeight);

        if (tiles.empty())
        {
            renderArea = rectangle;
        }
        else
        {
            renderArea.extend(rectangle);
        }

        tiles.push_back(rectangle);
    }

    if (_docType != "text" && part != _loKitDocument->pClass->getPart(_loKitDocument))
    {
        _loKitDocument->pClass->setPart(_loKitDocument, part);
    }

    LibreOfficeKitTileMode mode = static_cast<LibreOfficeKitTileMode>(_loKitDocument->pClass->getTileMode(_loKitDocument));

    int tilesByX = renderArea.getWidth() / tileWidth;
    int tilesByY = renderArea.getHeight() / tileHeight;

    int pixmapWidth = tilesByX * pixelWidth;
    int pixmapHeight = tilesByY * pixelHeight;

    const size_t pixmapSize = 4 * pixmapWidth * pixmapHeight;

    std::vector<unsigned char> pixmap(pixmapSize, 0);

    Poco::Timestamp timestamp;
    _loKitDocument->pClass->paintTile(_loKitDocument, pixmap.data(), pixmapWidth, pixmapHeight,
                                      renderArea.getLeft(), renderArea.getTop(),
                                      renderArea.getWidth(), renderArea.getHeight());

    std::cout << Util::logPrefix() << "paintTile (Multiple) called, tile at [" << renderArea.getLeft() << ", " << renderArea.getTop() << "]"
                << " (" << renderArea.getWidth() << ", " << renderArea.getHeight() << ") rendered in "
                << double(timestamp.elapsed())/1000 <<  "ms" << std::endl;

    for (Util::Rectangle& tileRect : tiles)
    {
        std::string response = "tile: part=" + std::to_string(part) +
                               " width=" + std::to_string(pixelWidth) +
                               " height=" + std::to_string(pixelHeight) +
                               " tileposx=" + std::to_string(tileRect.getLeft()) +
                               " tileposy=" + std::to_string(tileRect.getTop()) +
                               " tilewidth=" + std::to_string(tileWidth) +
                               " tileheight=" + std::to_string(tileHeight) + "\n";

        std::vector<char> output;
        output.reserve(pixelWidth * pixelHeight * 4 + response.size());
        output.resize(response.size());

        std::copy(response.begin(), response.end(), output.begin());

        int positionX = (tileRect.getLeft() - renderArea.getLeft()) / tileWidth;
        int positionY = (tileRect.getTop() - renderArea.getTop())  / tileHeight;

        if (!Util::encodeSubBufferToPNG(pixmap.data(), positionX * pixelWidth, positionY * pixelHeight, pixelWidth, pixelHeight, pixmapWidth, pixmapHeight, output, mode))
        {
            sendTextFrame("error: cmd=tile kind=failure");
            return;
        }

        sendBinaryFrame(output.data(), output.size());
    }
}

bool ChildProcessSession::clientZoom(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    int tilePixelWidth, tilePixelHeight, tileTwipWidth, tileTwipHeight;

    if (tokens.count() != 5 ||
        !getTokenInteger(tokens[1], "tilepixelwidth", tilePixelWidth) ||
        !getTokenInteger(tokens[2], "tilepixelheight", tilePixelHeight) ||
        !getTokenInteger(tokens[3], "tiletwipwidth", tileTwipWidth) ||
        !getTokenInteger(tokens[4], "tiletwipheight", tileTwipHeight))
    {
        sendTextFrame("error: cmd=clientzoom kind=syntax");
        return false;
    }

    _loKitDocument->pClass->setClientZoom(_loKitDocument, tilePixelWidth, tilePixelHeight, tileTwipWidth, tileTwipHeight);
    return true;
}
bool ChildProcessSession::downloadAs(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    std::string name, id, format, filterOptions;

    if (tokens.count() < 5 ||
        !getTokenString(tokens[1], "name", name) ||
        !getTokenString(tokens[2], "id", id))
    {
        sendTextFrame("error: cmd=downloadas kind=syntax");
        return false;
    }

    getTokenString(tokens[3], "format", format);

    if (getTokenString(tokens[4], "options", filterOptions))
    {
        if (tokens.count() > 5)
        {
            filterOptions += Poco::cat(std::string(" "), tokens.begin() + 5, tokens.end());
        }
    }

    std::string tmpDir, url;
    File *file = NULL;
    do
    {
        if (file != NULL)
        {
            delete file;
        }
        tmpDir = std::to_string((((Poco::UInt64)LOOLWSD::_rng.next()) << 32) | LOOLWSD::_rng.next() | 1);
        url = jailDocumentURL + "/" + tmpDir + "/" + name;
        file = new File(url);
    } while (file->exists());
    delete file;

    _loKitDocument->pClass->saveAs(_loKitDocument, url.c_str(),
            format.size() == 0 ? NULL :format.c_str(),
            filterOptions.size() == 0 ? NULL : filterOptions.c_str());

    sendTextFrame("downloadas: jail=" + _childId + " dir=" + tmpDir + " name=" + name +
            " port=" + std::to_string(LOOLWSD::portNumber) + " id=" + id);
    return true;
}

bool ChildProcessSession::getChildId()
{
    sendTextFrame("getchildid: id=" + _childId);
    return true;
}

bool ChildProcessSession::getTextSelection(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    std::string mimeType;

    if (tokens.count() != 2 ||
        !getTokenString(tokens[1], "mimetype", mimeType))
    {
        sendTextFrame("error: cmd=gettextselection kind=syntax");
        return false;
    }

    char *textSelection = _loKitDocument->pClass->getTextSelection(_loKitDocument, mimeType.c_str(), NULL);

    sendTextFrame("textselectioncontent: " + std::string(textSelection));
    return true;
}

bool ChildProcessSession::paste(const char* buffer, int length, StringTokenizer& tokens)
{
    std::string mimeType;

    if (tokens.count() < 2 || !getTokenString(tokens[1], "mimetype", mimeType))
    {
        sendTextFrame("error: cmd=paste kind=syntax");
        return false;
    }

    const std::string firstLine = getFirstLine(buffer, length);
    const char* data = buffer + firstLine.size() + 1;
    size_t size = length - firstLine.size() - 1;

    _loKitDocument->pClass->paste(_loKitDocument, mimeType.c_str(), data, size);

    return true;
}

bool ChildProcessSession::insertFile(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    std::string name, type;

    if (tokens.count() != 3 ||
        !getTokenString(tokens[1], "name", name) ||
        !getTokenString(tokens[2], "type", type))
    {
        sendTextFrame("error: cmd=insertfile kind=syntax");
        return false;
    }

    if (type == "graphic")
    {
        std::string fileName = "file://" + jailDocumentURL + "/insertfile/" + name;
        std::string command = ".uno:InsertGraphic";
        std::string arguments = "{"
            "\"FileName\":{"
                "\"type\":\"string\","
                "\"value\":\"" + fileName + "\""
            "}}";
        _loKitDocument->pClass->postUnoCommand(_loKitDocument, command.c_str(), arguments.c_str(), false);
    }

    return true;
}

bool ChildProcessSession::keyEvent(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    int type, charcode, keycode;

    if (tokens.count() != 4 ||
        !getTokenKeyword(tokens[1], "type",
                         {{"input", LOK_KEYEVENT_KEYINPUT}, {"up", LOK_KEYEVENT_KEYUP}},
                         type) ||
        !getTokenInteger(tokens[2], "char", charcode) ||
        !getTokenInteger(tokens[3], "key", keycode))
    {
        sendTextFrame("error: cmd=key kind=syntax");
        return false;
    }

    _loKitDocument->pClass->postKeyEvent(_loKitDocument, type, charcode, keycode);

    return true;
}

bool ChildProcessSession::mouseEvent(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    int type, x, y, count, buttons, modifier;

    if (tokens.count() != 7 ||
        !getTokenKeyword(tokens[1], "type",
                         {{"buttondown", LOK_MOUSEEVENT_MOUSEBUTTONDOWN},
                          {"buttonup", LOK_MOUSEEVENT_MOUSEBUTTONUP},
                          {"move", LOK_MOUSEEVENT_MOUSEMOVE}},
                         type) ||
        !getTokenInteger(tokens[2], "x", x) ||
        !getTokenInteger(tokens[3], "y", y) ||
        !getTokenInteger(tokens[4], "count", count) ||
        !getTokenInteger(tokens[5], "buttons", buttons) ||
        !getTokenInteger(tokens[6], "modifier", modifier))
    {
        sendTextFrame("error: cmd=mouse kind=syntax");
        return false;
    }

    _loKitDocument->pClass->postMouseEvent(_loKitDocument, type, x, y, count, buttons, modifier);

    return true;
}

bool ChildProcessSession::unoCommand(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    if (tokens.count() == 1)
    {
        sendTextFrame("error: cmd=uno kind=syntax");
        return false;
    }

    // we need to get LOK_CALLBACK_UNO_COMMAND_RESULT callback when saving
    bool bNotify = (tokens[1] == ".uno:Save");

    if (tokens.count() == 2)
    {
        _loKitDocument->pClass->postUnoCommand(_loKitDocument, tokens[1].c_str(), 0, bNotify);
    }
    else
    {
        _loKitDocument->pClass->postUnoCommand(_loKitDocument, tokens[1].c_str(), Poco::cat(std::string(" "), tokens.begin() + 2, tokens.end()).c_str(), bNotify);
    }

    return true;
}

bool ChildProcessSession::selectText(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    int type, x, y;

    if (tokens.count() != 4 ||
        !getTokenKeyword(tokens[1], "type",
                         {{"start", LOK_SETTEXTSELECTION_START},
                          {"end", LOK_SETTEXTSELECTION_END},
                          {"reset", LOK_SETTEXTSELECTION_RESET}},
                         type) ||
        !getTokenInteger(tokens[2], "x", x) ||
        !getTokenInteger(tokens[3], "y", y))
    {
        sendTextFrame("error: cmd=selecttext kind=syntax");
        return false;
    }

    _loKitDocument->pClass->setTextSelection(_loKitDocument, type, x, y);

    return true;
}

bool ChildProcessSession::selectGraphic(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    int type, x, y;

    if (tokens.count() != 4 ||
        !getTokenKeyword(tokens[1], "type",
                         {{"start", LOK_SETGRAPHICSELECTION_START},
                          {"end", LOK_SETGRAPHICSELECTION_END}},
                         type) ||
        !getTokenInteger(tokens[2], "x", x) ||
        !getTokenInteger(tokens[3], "y", y))
    {
        sendTextFrame("error: cmd=selectgraphic kind=syntax");
        return false;
    }

    _loKitDocument->pClass->setGraphicSelection(_loKitDocument, type, x, y);

    return true;
}

bool ChildProcessSession::resetSelection(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    if (tokens.count() != 1)
    {
        sendTextFrame("error: cmd=resetselection kind=syntax");
        return false;
    }

    _loKitDocument->pClass->resetSelection(_loKitDocument);

    return true;
}

bool ChildProcessSession::saveAs(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    std::string url, format, filterOptions;

    if (tokens.count() < 4 ||
        !getTokenString(tokens[1], "url", url))
    {
        sendTextFrame("error: cmd=saveas kind=syntax");
        return false;
    }

    getTokenString(tokens[2], "format", format);

    if (getTokenString(tokens[3], "options", filterOptions))
    {
        if (tokens.count() > 4)
        {
            filterOptions += Poco::cat(std::string(" "), tokens.begin() + 4, tokens.end());
        }
    }

    bool success = _loKitDocument->pClass->saveAs(_loKitDocument, url.c_str(),
            format.size() == 0 ? NULL :format.c_str(),
            filterOptions.size() == 0 ? NULL : filterOptions.c_str());

    sendTextFrame("saveas: url=" + url);
    std::string successStr = success ? "true" : "false";
    sendTextFrame("unocommandresult: {"
            "\"commandName\":\"saveas\","
            "\"success\":\"" + successStr + "\"}");

    return true;
}

bool ChildProcessSession::setClientPart(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    if (tokens.count() < 2 ||
        !getTokenInteger(tokens[1], "part", _clientPart))
    {
        return false;
    }
    return true;
}

bool ChildProcessSession::setPage(const char* /*buffer*/, int /*length*/, StringTokenizer& tokens)
{
    int page;
    if (tokens.count() < 2 ||
        !getTokenInteger(tokens[1], "page", page))
    {
        sendTextFrame("error: cmd=setpage kind=invalid");
        return false;
    }
    _loKitDocument->pClass->setPart(_loKitDocument, page);
    return true;
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
