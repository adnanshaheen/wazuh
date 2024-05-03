/*
 * Socket DB Wrapper
 * Copyright (C) 2015, Wazuh Inc.
 * October 30, 2023.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#ifndef _SOCKET_DB_WRAPPER_HPP
#define _SOCKET_DB_WRAPPER_HPP

#include "json.hpp"
#include "socketClient.hpp"
#include "socketDBWrapperException.hpp"
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>

auto constexpr DB_WRAPPER_QUERY_WAIT_TIME {5000};

char constexpr DB_WRAPPER_OK[] = {"ok"};
char constexpr DB_WRAPPER_ERROR[] = {"err"};
char constexpr DB_WRAPPER_UNKNOWN[] {"unk"};
char constexpr DB_WRAPPER_IGNORE[] {"ign"};
char constexpr DB_WRAPPER_DUE[] {"due"};

enum class DbQueryStatus : uint8_t
{
    UNKNOWN,
    JSON_PARSING,
    EMPTY_RESPONSE,
    QUERY_ERROR,
    QUERY_IGNORE,
    QUERY_UNKNOWN,
    QUERY_NOT_SYNCED,
    INVALID_RESPONSE
};

class SocketDBWrapper final
{
private:
    std::unique_ptr<SocketClient<Socket<OSPrimitives, SizeHeaderProtocol>, EpollWrapper>> m_dbSocket;
    nlohmann::json m_response;
    nlohmann::json m_responsePartial;
    std::string m_exceptionStr;
    DbQueryStatus m_queryStatus {DbQueryStatus::UNKNOWN};
    std::mutex m_mutexMessage;
    std::mutex m_mutexResponse;
    std::condition_variable m_conditionVariable;
    std::string m_socketPath;

    void initializeSocket()
    {
        m_dbSocket =
            std::make_unique<SocketClient<Socket<OSPrimitives, SizeHeaderProtocol>, EpollWrapper>>(m_socketPath);
        m_dbSocket->connect(
            [&](const char* body, uint32_t bodySize, const char*, uint32_t)
            {
                std::scoped_lock lock {m_mutexResponse};
                std::string responsePacket(body, bodySize);

                if (0 == responsePacket.compare(0, sizeof(DB_WRAPPER_DUE) - 1, DB_WRAPPER_DUE))
                {
                    try
                    {
                        m_responsePartial.push_back(nlohmann::json::parse(responsePacket.substr(4)));
                    }
                    catch (const nlohmann::detail::exception& ex)
                    {
                        m_queryStatus = DbQueryStatus::JSON_PARSING;
                        m_exceptionStr = "Error parsing JSON response: " + responsePacket.substr(4) +
                                         ". Exception id: " + std::to_string(ex.id) + ". " + ex.what();
                    }
                }
                else
                {
                    if (responsePacket.empty())
                    {
                        m_queryStatus = DbQueryStatus::EMPTY_RESPONSE;
                        m_exceptionStr = "Empty DB response";
                    }
                    else if (0 == responsePacket.compare(0, sizeof(DB_WRAPPER_ERROR) - 1, DB_WRAPPER_ERROR))
                    {
                        m_queryStatus = DbQueryStatus::QUERY_ERROR;
                        m_exceptionStr = "DB query error: " + responsePacket.substr(sizeof(DB_WRAPPER_ERROR));
                    }
                    else if (0 == responsePacket.compare(0, sizeof(DB_WRAPPER_IGNORE) - 1, DB_WRAPPER_IGNORE))
                    {
                        m_queryStatus = DbQueryStatus::QUERY_IGNORE;
                        m_exceptionStr = "DB query ignored: " + responsePacket.substr(sizeof(DB_WRAPPER_IGNORE));
                    }
                    else if (0 == responsePacket.compare(0, sizeof(DB_WRAPPER_UNKNOWN) - 1, DB_WRAPPER_UNKNOWN))
                    {
                        m_queryStatus = DbQueryStatus::QUERY_UNKNOWN;
                        m_exceptionStr =
                            "DB query unknown response: " + responsePacket.substr(sizeof(DB_WRAPPER_UNKNOWN));
                    }
                    else if (0 == responsePacket.compare(0, sizeof(DB_WRAPPER_OK) - 1, DB_WRAPPER_OK))
                    {
                        if (!m_responsePartial.empty())
                        {
                            m_response = m_responsePartial;
                        }
                        else
                        {
                            try
                            {
                                nlohmann::json responseParsed =
                                    nlohmann::json::parse(responsePacket.substr(sizeof(DB_WRAPPER_OK) - 1));
                                if (responseParsed.type() == nlohmann::json::value_t::array)
                                {
                                    m_response = std::move(responseParsed);
                                }
                                else
                                {
                                    if (responseParsed.contains("status") &&
                                        responseParsed.at("status") == "NOT_SYNCED")
                                    {
                                        m_queryStatus = DbQueryStatus::QUERY_NOT_SYNCED;
                                        m_exceptionStr = "DB query not synced";
                                    }
                                    else
                                    {
                                        m_response.push_back(responseParsed);
                                    }
                                }
                            }
                            catch (const nlohmann::detail::exception& ex)
                            {
                                m_queryStatus = DbQueryStatus::JSON_PARSING;
                                m_exceptionStr =
                                    "Error parsing JSON response: " + responsePacket.substr(sizeof(DB_WRAPPER_OK) - 1) +
                                    ". Exception id: " + std::to_string(ex.id) + ". " + ex.what();
                            }
                        }
                    }
                    else
                    {
                        m_queryStatus = DbQueryStatus::INVALID_RESPONSE;
                        m_exceptionStr = "DB query invalid response: " + responsePacket;
                    }
                    m_conditionVariable.notify_one();
                }
            });
    }

public:
    explicit SocketDBWrapper(std::string socketPath)
        : m_socketPath(std::move(socketPath))
    {
        initializeSocket();
    }

    void query(const std::string& query, nlohmann::json& response)
    {
        // Acquire lock to avoid multiple threads sending queries at the same time
        std::scoped_lock lockMessage {m_mutexMessage};

        // Acquire lock before clearing the response
        std::unique_lock lockResponse {m_mutexResponse};

        if (!m_dbSocket)
        {
            initializeSocket();
        }

        m_response.clear();
        m_responsePartial.clear();
        // coverity[missing_lock]
        m_exceptionStr.clear();

        m_dbSocket->send(query.c_str(), query.size());
        if (const auto res =
                m_conditionVariable.wait_for(lockResponse, std::chrono::milliseconds(DB_WRAPPER_QUERY_WAIT_TIME));
            res == std::cv_status::timeout)
        {
            // Restart the socket connection to avoid the reception of old messages
            m_dbSocket->stop();
            initializeSocket();
            throw std::runtime_error("Timeout waiting for DB response");
        }

        if (!m_exceptionStr.empty())
        {
            // coverity[missing_lock]
            switch (m_queryStatus)
            {
                case DbQueryStatus::EMPTY_RESPONSE:
                case DbQueryStatus::QUERY_ERROR:
                case DbQueryStatus::QUERY_IGNORE:
                case DbQueryStatus::QUERY_UNKNOWN:
                case DbQueryStatus::QUERY_NOT_SYNCED: throw SocketDbWrapperException(m_exceptionStr); break;
                default: throw std::runtime_error(m_exceptionStr);
            }
        }

        response = m_response;
    }
};

#endif // _SOCKET_DB_WRAPPER_HPP
