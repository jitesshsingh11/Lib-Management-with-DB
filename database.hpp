/*
 * database.hpp - SQLite storage layer shared by the console app (main.cpp)
 * and the web server (server.cpp). Both programs use the same library.db
 * file, so changes made in one show up in the other.
 *
 * All SQL lives in this file; the rest of the code just calls the methods.
 */
#ifndef DATABASE_HPP
#define DATABASE_HPP

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "models.hpp"
#include "sqlite3.h"

class Database {
    sqlite3 *db = nullptr;

    // Small RAII wrapper around a prepared statement.
    // Placeholders are numbered from 1, result columns from 0.
    class Stmt {
        sqlite3_stmt *st = nullptr;
        sqlite3 *db;

    public:
        Stmt(sqlite3 *db, const string &sql) : db(db) {
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) !=
                SQLITE_OK)
                throw runtime_error(string("SQL error: ") + sqlite3_errmsg(db));
        }
        ~Stmt() { sqlite3_finalize(st); }
        Stmt(const Stmt &) = delete;
        Stmt &operator=(const Stmt &) = delete;

        Stmt &num(int i, long long v) {
            sqlite3_bind_int64(st, i, v);
            return *this;
        }
        Stmt &text(int i, const string &s) {
            sqlite3_bind_text(st, i, s.c_str(), (int)s.size(), SQLITE_TRANSIENT);
            return *this;
        }
        Stmt &null(int i) {
            sqlite3_bind_null(st, i);
            return *this;
        }

        // true = a result row is ready, false = finished
        bool row() {
            int rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) return true;
            if (rc == SQLITE_DONE) return false;
            throw runtime_error(string("SQL error: ") + sqlite3_errmsg(db));
        }
        void run() { row(); }

        long long i64(int c) { return sqlite3_column_int64(st, c); }
        string str(int c) {
            const unsigned char *p = sqlite3_column_text(st, c);
            return p ? string(reinterpret_cast<const char *>(p)) : string();
        }
    };

    void exec(const string &sql) {
        char *err = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            string msg = err ? err : "unknown error";
            sqlite3_free(err);
            throw runtime_error("SQL error: " + msg);
        }
    }

    bool exists(const string &table, int id) {
        Stmt s(db, "SELECT 1 FROM " + table + " WHERE id = ?");
        return s.num(1, id).row();
    }

    // One-time import of the old books.txt / members.txt files so existing
    // data is not lost. Runs only on a brand-new database; the text files
    // are left untouched as a backup.
    void importLegacyFiles() {
        ifstream mf(MEMBERS_FILE), bf(BOOKS_FILE);
        exec("BEGIN");
        try {
            string line;
            while (getline(mf, line)) {
                Member m(0, "");
                if (Member::deserialize(line, m))
                    Stmt(db, "INSERT OR IGNORE INTO members(id, name) VALUES(?, ?)")
                        .num(1, m.getId())
                        .text(2, m.getName())
                        .run();
            }
            while (getline(bf, line)) {
                Book b(0, "", "");
                if (!Book::deserialize(line, b)) continue;
                // a book issued to a member that no longer exists comes in as available
                bool issued = b.isIssued() && exists("members", b.getMemberId());
                Stmt s(db,
                       "INSERT OR IGNORE INTO books(id, title, author, issued, "
                       "member_id, issue_time) VALUES(?, ?, ?, ?, ?, ?)");
                s.num(1, b.getId()).text(2, b.getTitle()).text(3, b.getAuthor());
                s.num(4, issued ? 1 : 0);
                if (issued)
                    s.num(5, b.getMemberId()).num(6, (long long)b.getIssueTime());
                else
                    s.null(5).num(6, 0);
                s.run();
            }
            exec("COMMIT");
        } catch (...) {
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }

public:
    Database() = default;
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    ~Database() {
        if (db) sqlite3_close(db);
    }

    // Opens (and if needed creates) the database file and its tables.
    void open(const string &path = DB_FILE) {
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
            string msg = db ? sqlite3_errmsg(db) : "out of memory";
            sqlite3_close(db);
            db = nullptr;
            throw runtime_error("Cannot open database '" + path + "': " + msg);
        }
        sqlite3_busy_timeout(db, 5000);  // wait if the other program is writing
        exec("PRAGMA foreign_keys = ON");
        exec("CREATE TABLE IF NOT EXISTS members ("
             "  id   INTEGER PRIMARY KEY,"
             "  name TEXT NOT NULL)");
        exec("CREATE TABLE IF NOT EXISTS books ("
             "  id         INTEGER PRIMARY KEY,"
             "  title      TEXT NOT NULL,"
             "  author     TEXT NOT NULL,"
             "  issued     INTEGER NOT NULL DEFAULT 0 CHECK (issued IN (0, 1)),"
             "  member_id  INTEGER REFERENCES members(id),"
             "  issue_time INTEGER NOT NULL DEFAULT 0)");

        long long version = 0;
        {
            Stmt s(db, "PRAGMA user_version");
            if (s.row()) version = s.i64(0);
        }
        if (version == 0) {
            importLegacyFiles();
            exec("PRAGMA user_version = 1");
        }
    }

    // ---- reading
    void loadBooks(vector<Book> &out) {
        out.clear();
        Stmt s(db,
               "SELECT id, title, author, issued, member_id, issue_time "
               "FROM books ORDER BY id");
        while (s.row()) {
            Book b((int)s.i64(0), s.str(1), s.str(2));
            if (s.i64(3) == 1) b.issueTo((int)s.i64(4), (time_t)s.i64(5));
            out.push_back(b);
        }
    }

    void loadMembers(vector<Member> &out) {
        out.clear();
        Stmt s(db, "SELECT id, name FROM members ORDER BY id");
        while (s.row()) out.emplace_back((int)s.i64(0), s.str(1));
    }

    // ---- changes (each returns the new ID or whether a row was changed)
    int insertBook(const string &title, const string &author) {
        Stmt(db, "INSERT INTO books(title, author) VALUES(?, ?)")
            .text(1, title)
            .text(2, author)
            .run();
        return (int)sqlite3_last_insert_rowid(db);
    }

    int insertMember(const string &name) {
        Stmt(db, "INSERT INTO members(name) VALUES(?)").text(1, name).run();
        return (int)sqlite3_last_insert_rowid(db);
    }

    // false if the book is currently issued
    bool deleteBook(int id) {
        Stmt(db, "DELETE FROM books WHERE id = ? AND issued = 0")
            .num(1, id)
            .run();
        return sqlite3_changes(db) > 0;
    }

    // false if the book is already issued
    bool issueBook(int bookId, int memberId, time_t when) {
        Stmt(db,
             "UPDATE books SET issued = 1, member_id = ?, issue_time = ? "
             "WHERE id = ? AND issued = 0")
            .num(1, memberId)
            .num(2, (long long)when)
            .num(3, bookId)
            .run();
        return sqlite3_changes(db) > 0;
    }

    // false if the book was not issued
    bool returnBook(int bookId) {
        Stmt(db,
             "UPDATE books SET issued = 0, member_id = NULL, issue_time = 0 "
             "WHERE id = ? AND issued = 1")
            .num(1, bookId)
            .run();
        return sqlite3_changes(db) > 0;
    }
};

#endif
