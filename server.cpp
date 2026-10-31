/*
 * Library Management System - Web Server
 * A tiny HTTP server written in C++ (plain sockets, no libraries).
 * It serves index.html and a JSON API, and uses the same Book/Member
 * classes and SQLite database (library.db) as the console version.
 *
 * Build (Linux / macOS / WSL):
 *     g++ -std=c++17 -Wall -pthread -o server server.cpp -lsqlite3
 * Build (Windows, MinGW):
 *     g++ -std=c++17 -o server.exe server.cpp -lsqlite3 -lws2_32
 *
 * Run:   ./server [port]        (default port 8080)
 * Open:  http://localhost:8080
 */

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <cstdint>
#include <string>
#include <vector>

#include "database.hpp"  // SQLite storage (also pulls in models.hpp)

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define CLOSE_SOCKET closesocket
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define CLOSE_SOCKET close
#define INVALID_SOCKET (-1)
#endif

// ------------------------------------------------------------ small utils

static string trim(const string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static string urlDecode(const string &s) {
    string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() &&
                   isxdigit((unsigned char)s[i + 1]) &&
                   isxdigit((unsigned char)s[i + 2])) {
            out += (char)stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

static map<string, string> parseForm(const string &s) {
    map<string, string> m;
    for (const string &pair : split(s, '&')) {
        size_t eq = pair.find('=');
        if (eq == string::npos) continue;
        m[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
    }
    return m;
}

static string jsonEscape(const string &s) {
    string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// ------------------------------------------------------ request / response

struct Request {
    string method, path;
    map<string, string> query, form;
};

struct Response {
    int status = 200;
    string type = "application/json";
    string body;
};

static Response jsonError(const string &msg, int status = 400) {
    return {status, "application/json",
            "{\"error\":\"" + jsonEscape(msg) + "\"}"};
}

static Response jsonOk(const string &body) {
    return {200, "application/json", body};
}

// ------------------------------------------------------------------ Store

class Store {
    vector<Book> books;
    vector<Member> members;
    Database db;

    Book *findBook(int id) {
        for (auto &b : books)
            if (b.getId() == id) return &b;
        return nullptr;
    }

    Member *findMember(int id) {
        for (auto &m : members)
            if (m.getId() == id) return &m;
        return nullptr;
    }

    static int daysHeld(const Book &b) {
        return (int)(difftime(time(nullptr), b.getIssueTime()) / (24 * 3600));
    }

    static int overdueDays(const Book &b) {
        return b.isIssued() ? max(0, daysHeld(b) - LOAN_DAYS) : 0;
    }

    static bool getInt(const map<string, string> &m, const string &key,
                       int &out) {
        auto it = m.find(key);
        if (it == m.end()) return false;
        try {
            size_t pos;
            out = stoi(it->second, &pos);
            return pos == it->second.size();
        } catch (...) {
            return false;
        }
    }

    static string cleanField(string s) {
        s = trim(s);
        replace(s.begin(), s.end(), '\n', ' ');
        replace(s.begin(), s.end(), '\r', ' ');
        return s;
    }

public:
    void open() { db.open(); }

    // (re)read everything from the database so the web page always shows
    // changes made by the console app too
    void load() {
        db.loadBooks(books);
        db.loadMembers(members);
    }

    // ---- reading

    Response listBooks() {
        string out = "[";
        for (size_t i = 0; i < books.size(); i++) {
            const Book &b = books[i];
            Member *m = b.isIssued() ? findMember(b.getMemberId()) : nullptr;
            if (i) out += ",";
            out += "{\"id\":" + to_string(b.getId()) + ",\"title\":\"" +
                   jsonEscape(b.getTitle()) + "\",\"author\":\"" +
                   jsonEscape(b.getAuthor()) + "\",\"issued\":" +
                   (b.isIssued() ? "true" : "false");
            if (b.isIssued()) {
                out += ",\"memberId\":" + to_string(b.getMemberId()) +
                       ",\"memberName\":\"" +
                       jsonEscape(m ? m->getName() : "Unknown") +
                       "\",\"issuedOn\":\"" + formatDate(b.getIssueTime()) +
                       "\",\"dueOn\":\"" +
                       formatDate(b.getIssueTime() + LOAN_DAYS * 24 * 3600) +
                       "\",\"overdueDays\":" + to_string(overdueDays(b)) +
                       ",\"fine\":" +
                       to_string(overdueDays(b) * FINE_PER_DAY);
            }
            out += "}";
        }
        return jsonOk(out + "]");
    }

    Response listMembers() {
        string out = "[";
        for (size_t i = 0; i < members.size(); i++) {
            int count = 0;
            for (auto &b : books)
                if (b.isIssued() && b.getMemberId() == members[i].getId())
                    count++;
            if (i) out += ",";
            out += "{\"id\":" + to_string(members[i].getId()) +
                   ",\"name\":\"" + jsonEscape(members[i].getName()) +
                   "\",\"borrowed\":" + to_string(count) + "}";
        }
        return jsonOk(out + "]");
    }

    Response stats() {
        int issued = 0, overdue = 0;
        for (auto &b : books) {
            if (b.isIssued()) issued++;
            if (overdueDays(b) > 0) overdue++;
        }
        return jsonOk("{\"books\":" + to_string(books.size()) +
                      ",\"available\":" + to_string(books.size() - issued) +
                      ",\"issued\":" + to_string(issued) +
                      ",\"overdue\":" + to_string(overdue) +
                      ",\"members\":" + to_string(members.size()) +
                      ",\"loanDays\":" + to_string(LOAN_DAYS) +
                      ",\"finePerDay\":" + to_string(FINE_PER_DAY) + "}");
    }

    // ---- changes

    Response addBook(const map<string, string> &f) {
        string title = cleanField(f.count("title") ? f.at("title") : "");
        string author = cleanField(f.count("author") ? f.at("author") : "");
        if (title.empty() || author.empty())
            return jsonError("Title and author are required.");
        int id = db.insertBook(title, author);
        books.emplace_back(id, title, author);
        return jsonOk("{\"message\":\"Book added with ID " + to_string(id) +
                      ".\"}");
    }

    Response removeBook(const map<string, string> &f) {
        int id;
        if (!getInt(f, "id", id)) return jsonError("Invalid book ID.");
        auto it = find_if(books.begin(), books.end(),
                          [id](const Book &b) { return b.getId() == id; });
        if (it == books.end()) return jsonError("No such book.", 404);
        if (it->isIssued())
            return jsonError("Cannot remove: book is currently issued.");
        if (!db.deleteBook(id))
            return jsonError("Cannot remove: book is currently issued.");
        books.erase(it);
        return jsonOk("{\"message\":\"Book removed.\"}");
    }

    Response addMember(const map<string, string> &f) {
        string name = cleanField(f.count("name") ? f.at("name") : "");
        if (name.empty()) return jsonError("Member name is required.");
        int id = db.insertMember(name);
        members.emplace_back(id, name);
        return jsonOk("{\"message\":\"Member registered with ID " +
                      to_string(id) + ".\"}");
    }

    Response issueBook(const map<string, string> &f) {
        int bid, mid;
        if (!getInt(f, "bookId", bid) || !getInt(f, "memberId", mid))
            return jsonError("Book and member are required.");
        Book *b = findBook(bid);
        Member *m = findMember(mid);
        if (!b) return jsonError("No such book.", 404);
        if (!m) return jsonError("No such member.", 404);
        if (b->isIssued()) return jsonError("Book is already issued.");
        time_t now = time(nullptr);
        if (!db.issueBook(bid, mid, now))
            return jsonError("Book is already issued.");
        b->issueTo(mid, now);
        return jsonOk("{\"message\":\"Issued '" + jsonEscape(b->getTitle()) +
                      "' to " + jsonEscape(m->getName()) + ". Due on " +
                      formatDate(now + LOAN_DAYS * 24 * 3600) + ".\"}");
    }

    Response returnBook(const map<string, string> &f) {
        int bid;
        if (!getInt(f, "bookId", bid)) return jsonError("Invalid book ID.");
        Book *b = findBook(bid);
        if (!b) return jsonError("No such book.", 404);
        if (!b->isIssued()) return jsonError("That book was not issued.");
        int held = daysHeld(*b);
        int late = overdueDays(*b);
        int fine = late * FINE_PER_DAY;
        string title = b->getTitle();
        if (!db.returnBook(bid)) return jsonError("That book was not issued.");
        b->giveBack();
        string msg = "Returned '" + jsonEscape(title) + "'. ";
        msg += late > 0 ? "Late by " + to_string(late) + " day(s). Fine: Rs " +
                              to_string(fine) + "."
                        : "Returned on time. No fine.";
        return jsonOk("{\"message\":\"" + msg + "\",\"daysHeld\":" +
                      to_string(held) + ",\"lateDays\":" + to_string(late) +
                      ",\"fine\":" + to_string(fine) + "}");
    }
};

// A tiny mutex wrapper built on the operating system's own API, so the
// program does not depend on std::mutex / std::thread (some MinGW builds
// on Windows do not provide them).
class Mutex {
#ifdef _WIN32
    CRITICAL_SECTION cs;

public:
    Mutex() { InitializeCriticalSection(&cs); }
    ~Mutex() { DeleteCriticalSection(&cs); }
    void lock() { EnterCriticalSection(&cs); }
    void unlock() { LeaveCriticalSection(&cs); }
#else
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

public:
    void lock() { pthread_mutex_lock(&m); }
    void unlock() { pthread_mutex_unlock(&m); }
#endif
};

// Locks in the constructor, unlocks in the destructor (RAII)
class Lock {
    Mutex &mtx;

public:
    explicit Lock(Mutex &m) : mtx(m) { mtx.lock(); }
    ~Lock() { mtx.unlock(); }
};

static Store store;
static Mutex storeMutex;

// ----------------------------------------------------------------- routing

static string readFile(const string &name, bool &ok) {
    ifstream f(name, ios::binary);
    ok = f.good();
    stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static Response handle(const Request &req) {
    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
        bool ok;
        string html = readFile("index.html", ok);
        if (!ok)
            return {404, "text/plain",
                    "index.html not found. Run the server from the folder "
                    "that contains index.html."};
        return {200, "text/html", html};
    }

    Lock lock(storeMutex);

    try {
        store.load();  // pick up any changes made from the console app

        if (req.method == "GET" && req.path == "/api/books") return store.listBooks();
        if (req.method == "GET" && req.path == "/api/members") return store.listMembers();
        if (req.method == "GET" && req.path == "/api/stats") return store.stats();

        if (req.method == "POST") {
            if (req.path == "/api/books") return store.addBook(req.form);
            if (req.path == "/api/books/remove") return store.removeBook(req.form);
            if (req.path == "/api/members") return store.addMember(req.form);
            if (req.path == "/api/issue") return store.issueBook(req.form);
            if (req.path == "/api/return") return store.returnBook(req.form);
        }
    } catch (const exception &e) {
        return jsonError(string("Database error: ") + e.what(), 500);
    }
    return jsonError("Not found.", 404);
}

// ----------------------------------------------------------------- sockets

static const size_t MAX_REQUEST = 1 << 20;  // 1 MB

static bool readRequest(sock_t s, Request &req) {
    string data;
    char buf[4096];
    size_t headerEnd = string::npos;
    while (headerEnd == string::npos) {
        long n = (long)recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        data.append(buf, (size_t)n);
        if (data.size() > MAX_REQUEST) return false;
        headerEnd = data.find("\r\n\r\n");
    }

    string head = data.substr(0, headerEnd);
    string body = data.substr(headerEnd + 4);

    istringstream hs(head);
    string line;
    getline(hs, line);
    istringstream rl(line);
    string target, version;
    rl >> req.method >> target >> version;
    if (req.method.empty() || target.empty()) return false;

    size_t q = target.find('?');
    req.path = target.substr(0, q);
    if (q != string::npos) req.query = parseForm(target.substr(q + 1));

    size_t contentLength = 0;
    while (getline(hs, line)) {
        size_t c = line.find(':');
        if (c == string::npos) continue;
        if (toLower(trim(line.substr(0, c))) == "content-length") {
            try {
                contentLength = stoul(trim(line.substr(c + 1)));
            } catch (...) {
                return false;
            }
        }
    }
    if (contentLength > MAX_REQUEST) return false;

    while (body.size() < contentLength) {
        long n = (long)recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        body.append(buf, (size_t)n);
    }
    req.form = parseForm(body);
    return true;
}

static void sendAll(sock_t s, const string &data) {
    size_t off = 0;
    while (off < data.size()) {
        long n = (long)send(s, data.data() + off, data.size() - off, 0);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

static const char *statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        default: return "Error";
    }
}

static void serveClient(sock_t s) {
    Request req;
    if (readRequest(s, req)) {
        Response res = handle(req);
        string out = "HTTP/1.1 " + to_string(res.status) + " " +
                     statusText(res.status) + "\r\nContent-Type: " + res.type +
                     "; charset=utf-8\r\nContent-Length: " +
                     to_string(res.body.size()) +
                     "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" +
                     res.body;
        sendAll(s, out);
        cout << req.method << " " << req.path << " -> " << res.status << endl;
    }
    CLOSE_SOCKET(s);
}

// Start serveClient() on its own thread (native API on each platform).
// If a thread cannot be created, the request is handled right here instead.
#ifdef _WIN32
static DWORD WINAPI clientThread(LPVOID arg) {
    serveClient((sock_t)(uintptr_t)arg);
    return 0;
}

static void startThread(sock_t s) {
    HANDLE h = CreateThread(nullptr, 0, clientThread, (LPVOID)(uintptr_t)s, 0,
                            nullptr);
    if (h)
        CloseHandle(h);
    else
        serveClient(s);
}
#else
static void *clientThread(void *arg) {
    serveClient((sock_t)(intptr_t)arg);
    return nullptr;
}

static void startThread(sock_t s) {
    pthread_t t;
    if (pthread_create(&t, nullptr, clientThread, (void *)(intptr_t)s) == 0)
        pthread_detach(t);
    else
        serveClient(s);
}
#endif

int main(int argc, char *argv[]) {
    int port = 8080;
    if (argc > 1) {
        try {
            port = stoi(argv[1]);
        } catch (...) {
            cerr << "Usage: " << argv[0] << " [port]\n";
            return 1;
        }
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        cerr << "WSAStartup failed.\n";
        return 1;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    try {
        store.open();
    } catch (const exception &e) {
        cerr << e.what() << "\n";
        return 1;
    }

    sock_t server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) {
        cerr << "Could not create socket.\n";
        return 1;
    }
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this computer only
    addr.sin_port = htons((unsigned short)port);

    if (bind(server, (sockaddr *)&addr, sizeof(addr)) != 0) {
        cerr << "Could not bind to port " << port
             << " (is it already in use? try another port).\n";
        return 1;
    }
    if (listen(server, 16) != 0) {
        cerr << "listen() failed.\n";
        return 1;
    }

    cout << "Library server running at http://localhost:" << port
         << "\nPress Ctrl+C to stop.\n";

    while (true) {
        sock_t client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        // give up on clients that connect but never send anything
#ifdef _WIN32
        DWORD timeout = 5000;
#else
        timeval timeout{5, 0};
#endif
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
                   sizeof(timeout));

        startThread(client);
    }
}
