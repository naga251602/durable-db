#include <fcntl.h> // gives access to open()
#include <unistd.h> // give me access to write(), read(), close()
#include <cstdio> // give me access to perror()
#include <iostream> // std::cout
#include <memory>

// best practices always follow RAII practices when handling resources lifetimes
/*
 * Helps to avoid memory leaks, OS resources frozen
 * compiler takes of objects dev's like us need to take of memory
 */
class DURABLE_DB {
private:
        int f;
public:
    DURABLE_DB(const char *filename) {
        f = open(filename, O_RDWR | O_CREAT, 0644);
        if (f == -1) {
            perror("file open error");
        }
    }

    int getFd() {
        return this->f;
    }

    // methods to be implemented
    // void put() {}
    // string get() {}
    // void drop() {}

    ~DURABLE_DB() {
            if (f != -1) {close(f);}
    }
};

int main() {
    DURABLE_DB db = DURABLE_DB("test.txt");

    char buffer[7];
    read(db.getFd(), buffer, 7);

    std::cout.write(buffer, 7);
    return 0;
}