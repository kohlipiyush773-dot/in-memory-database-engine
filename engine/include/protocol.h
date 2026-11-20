// protocol.h - text command -> one line of JSON.  This is how the web server talks to the engine.
#pragma once
#include <string>
#include "database.h"

std::string HandleCommand(Database& db, const std::string& line, bool* quit);
std::string ReadyBanner(Database& db);
std::string ResultToJson(Database& db, const QueryResult& r);
std::string ResultToText(const QueryResult& r);       // for the --cli mode
