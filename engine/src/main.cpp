// main.cpp - PageDB engine executable.
//   pagedb --serve --db data/pagedb.db      JSON line protocol on stdin/stdout (used by the web UI)
//   pagedb --cli   --db data/pagedb.db      interactive SQL shell in the terminal
#include <cstdio>
#include <iostream>
#include "protocol.h"
#include "simulator.h"

int main(int argc, char** argv) {
    std::string path = "pagedb.db", policy = "LRU"; long long frames = 16; bool serve = false, cli = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--serve") serve = true;
        else if (a == "--cli") cli = true;
        else if (a == "--db" && i + 1 < argc) path = argv[++i];
        else if (a == "--policy" && i + 1 < argc) policy = argv[++i];
        else if (a == "--frames" && i + 1 < argc) ParseLL(argv[++i], &frames);
        else { std::fprintf(stderr, "usage: pagedb (--serve | --cli) [--db file] [--policy FIFO|LRU|CLOCK] [--frames N]\n"); return 2; }
    }
    if (!serve && !cli) cli = true;
    Database db;
    try { db.Open(path, (size_t)frames, policy); }
    catch (const std::exception& e) {
        if (serve) std::printf("{\"event\":\"error\",\"error\":%s}\n", Q(e.what()).c_str()); else std::fprintf(stderr, "cannot open database: %s\n", e.what());
        return 1;
    }
    std::string line; bool quit = false;
    if (serve) {
        std::printf("%s\n", ReadyBanner(db).c_str()); std::fflush(stdout);
        while (!quit && std::getline(std::cin, line)) {
            if (line.empty()) continue;
            std::printf("%s\n", HandleCommand(db, line, &quit).c_str());
            std::fflush(stdout);
        }
        db.Close();
        return 0;
    }
    std::printf("PageDB shell  (policy %s, %d frames).  Type SQL, or .help\n", db.policy().c_str(), (int)frames);
    if (db.recovery().ran) std::printf("[recovery] redo applied %ld, undo applied %ld, losers %ld\n", db.recovery().redo_applied, db.recovery().undo_applied, db.recovery().losers);
    while (!quit) {
        std::printf("pagedb> "); std::fflush(stdout);
        if (!std::getline(std::cin, line)) break;
        while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ')) line.erase(line.size() - 1);
        if (line.empty()) continue;
        if (line[0] == '.') {
            std::string c = line.substr(1);
            if (c == "quit" || c == "exit") break;
            if (c == "help") { std::printf(".pool  .seed N  .policy LRU|FIFO|CLOCK N  .flush  .crash  .quit   (anything else is SQL)\n"); continue; }
            if (c == "pool") { std::printf("%s\n", HandleCommand(db, "pool", &quit).c_str()); continue; }
            if (c.compare(0, 4, "seed") == 0) { std::printf("%s", ResultToText(db.Seed(c.size() > 5 ? std::atoi(c.c_str() + 5) : 100)).c_str()); continue; }
            if (c.compare(0, 6, "policy") == 0) { std::printf("%s\n", HandleCommand(db, "config" + c.substr(6), &quit).c_str()); continue; }
            if (c == "flush") { std::printf("%s", ResultToText(db.Admin("flush")).c_str()); continue; }
            if (c == "crash") { std::printf("simulating a crash (nothing is flushed)...\n"); db.Abandon(); return 0; }
            std::printf("unknown command, try .help\n"); continue;
        }
        std::printf("%s", ResultToText(db.Execute(line)).c_str());
    }
    db.Close();
    return 0;
}
