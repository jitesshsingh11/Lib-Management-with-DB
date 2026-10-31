/*
 * models.hpp - shared classes for the console app (main.cpp) and the
 * web server (server.cpp). Both programs store their data in the same
 * SQLite database (library.db, see database.hpp). The old books.txt /
 * members.txt files are only read once, to import existing data.
 */
#ifndef MODELS_HPP
#define MODELS_HPP

#include <algorithm>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

const int LOAN_DAYS = 14;
const int FINE_PER_DAY = 2;
const string DB_FILE = "library.db";
const string BOOKS_FILE = "books.txt";      // legacy, import only
const string MEMBERS_FILE = "members.txt";  // legacy, import only

// ---------------------------------------------------------------- helpers

inline vector<string> split(const string &s, char delim) {
    vector<string> parts;
    string item;
    stringstream ss(s);
    while (getline(ss, item, delim)) parts.push_back(item);
    return parts;
}

inline string toLower(string s) {
    transform(s.begin(), s.end(), s.begin(),
              [](unsigned char c) { return tolower(c); });
    return s;
}

inline string formatDate(time_t t) {
    char buf[32];
    strftime(buf, sizeof(buf), "%d-%m-%Y", localtime(&t));
    return buf;
}

// ------------------------------------------------------------------ Book

class Book {
    int id;
    string title;
    string author;
    bool issued;
    int memberId;      // who has it (0 if nobody)
    time_t issueTime;  // when it was issued

public:
    Book(int id, string title, string author)
        : id(id), title(move(title)), author(move(author)),
          issued(false), memberId(0), issueTime(0) {}

    int getId() const { return id; }
    const string &getTitle() const { return title; }
    const string &getAuthor() const { return author; }
    bool isIssued() const { return issued; }
    int getMemberId() const { return memberId; }
    time_t getIssueTime() const { return issueTime; }

    void issueTo(int member, time_t when) {
        issued = true;
        memberId = member;
        issueTime = when;
    }

    void giveBack() {
        issued = false;
        memberId = 0;
        issueTime = 0;
    }

    // text format of the old books.txt (used only for the one-time import)
    string serialize() const {
        return to_string(id) + "|" + title + "|" + author + "|" +
               to_string(issued ? 1 : 0) + "|" + to_string(memberId) + "|" +
               to_string(static_cast<long long>(issueTime));
    }

    static bool deserialize(const string &line, Book &out) {
        vector<string> f = split(line, '|');
        if (f.size() != 6) return false;
        try {
            Book b(stoi(f[0]), f[1], f[2]);
            if (stoi(f[3]) == 1) b.issueTo(stoi(f[4]), stoll(f[5]));
            out = b;
            return true;
        } catch (...) {
            return false;
        }
    }
};

// ---------------------------------------------------------------- Member

class Member {
    int id;
    string name;

public:
    Member(int id, string name) : id(id), name(move(name)) {}
    int getId() const { return id; }
    const string &getName() const { return name; }

    string serialize() const { return to_string(id) + "|" + name; }

    static bool deserialize(const string &line, Member &out) {
        vector<string> f = split(line, '|');
        if (f.size() != 2) return false;
        try {
            out = Member(stoi(f[0]), f[1]);
            return true;
        } catch (...) {
            return false;
        }
    }
};

#endif
