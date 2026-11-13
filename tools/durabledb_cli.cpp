// Tiny command line front end, handy for demos.
//   durabledb_cli <dir> put <key> <value>
//   durabledb_cli <dir> get <key>
//   durabledb_cli <dir> del <key>
//   durabledb_cli <dir> flush | compact | stats
#include <iostream>
#include <string>

#include "durabledb/db.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: durabledb_cli <dir> put|get|del|flush|compact|stats [args]\n";
    return 2;
  }
  try {
    auto db = durabledb::DB::Open(argv[1]);
    std::string cmd = argv[2];
    if (cmd == "put" && argc == 5) {
      return db->Put(argv[3], argv[4]) ? 0 : 1;
    } else if (cmd == "get" && argc == 4) {
      auto v = db->Get(argv[3]);
      if (!v) { std::cout << "(not found)\n"; return 1; }
      std::cout << *v << "\n";
    } else if (cmd == "del" && argc == 4) {
      return db->Delete(argv[3]) ? 0 : 1;
    } else if (cmd == "flush") {
      db->Flush();
    } else if (cmd == "compact") {
      db->Compact();
    } else if (cmd == "stats") {
      std::cout << "sstables: " << db->NumTables() << "\nmemtable bytes: " << db->MemTableBytes() << "\n";
    } else {
      std::cerr << "bad command\n";
      return 2;
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
