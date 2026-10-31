/*
 * Library Management System
 * College Mini Project - C++ (OOP, STL, SQL database)
 *
 * Features:
 *  - Add / remove / list / search books
 *  - Register / list members
 *  - Issue and return books (14-day loan, fine of Rs 2 per late day)
 *  - Data stored in an SQLite database (library.db)
 *
 * Build:  g++ -std=c++17 -Wall -o library main.cpp -lsqlite3
 *         (needs models.hpp and database.hpp alongside)
 * Run:    ./library
 */

#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "database.hpp"  // SQLite storage (also pulls in models.hpp)

using namespace std;

// ---------------------------------------------------------------- input

string readLine(const string &prompt) {
    string line;
    while (true) {
        cout << prompt;
        if (!getline(cin, line)) exit(0);  // end of input
        if (!line.empty()) return line;
        cout << "  Input cannot be empty.\n";
    }
}

int readInt(const string &prompt) {
    while (true) {
        string line = readLine(prompt);
        try {
            size_t pos;
            int v = stoi(line, &pos);
            if (pos == line.size()) return v;
        } catch (...) {
        }
        cout << "  Please enter a valid number.\n";
    }
}

// --------------------------------------------------------------- Library

class Library {
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

    string memberName(int id) {
        Member *m = findMember(id);
        return m ? m->getName() : "Unknown";
    }

    static void printBookHeader() {
        cout << left << setw(5) << "ID" << setw(30) << "Title" << setw(22)
             << "Author" << "Status\n";
        cout << string(75, '-') << "\n";
    }

    void printBook(const Book &b) {
        string status = "Available";
        if (b.isIssued())
            status = "Issued to " + memberName(b.getMemberId());
        cout << left << setw(5) << b.getId() << setw(30)
             << b.getTitle().substr(0, 28) << setw(22)
             << b.getAuthor().substr(0, 20) << status << "\n";
    }

public:
    // ---- persistence
    void open() { db.open(); }

    // (re)read everything from the database so the console and the web
    // server always see each other's changes
    void load() {
        db.loadBooks(books);
        db.loadMembers(members);
    }

    // ---- book operations
    void addBook() {
        string title = readLine("Title  : ");
        string author = readLine("Author : ");
        int id = db.insertBook(title, author);
        books.emplace_back(id, title, author);
        cout << "Book added with ID " << id << ".\n";
    }

    void removeBook() {
        int id = readInt("Book ID to remove: ");
        auto it = find_if(books.begin(), books.end(),
                          [id](const Book &b) { return b.getId() == id; });
        if (it == books.end()) {
            cout << "No such book.\n";
        } else if (it->isIssued()) {
            cout << "Cannot remove: book is currently issued.\n";
        } else if (!db.deleteBook(id)) {
            cout << "Cannot remove: book is currently issued.\n";
        } else {
            books.erase(it);
            cout << "Book removed.\n";
        }
    }

    void listBooks() {
        if (books.empty()) {
            cout << "No books in the library.\n";
            return;
        }
        printBookHeader();
        for (auto &b : books) printBook(b);
    }

    void searchBooks() {
        string q = toLower(readLine("Search title/author: "));
        bool found = false;
        for (auto &b : books) {
            if (toLower(b.getTitle()).find(q) != string::npos ||
                toLower(b.getAuthor()).find(q) != string::npos) {
                if (!found) printBookHeader();
                printBook(b);
                found = true;
            }
        }
        if (!found) cout << "No matching books.\n";
    }

    // ---- member operations
    void addMember() {
        string name = readLine("Member name: ");
        int id = db.insertMember(name);
        members.emplace_back(id, name);
        cout << "Member registered with ID " << id << ".\n";
    }

    void listMembers() {
        if (members.empty()) {
            cout << "No members registered.\n";
            return;
        }
        cout << left << setw(5) << "ID" << setw(25) << "Name"
             << "Books borrowed\n";
        cout << string(50, '-') << "\n";
        for (auto &m : members) {
            int count = 0;
            for (auto &b : books)
                if (b.isIssued() && b.getMemberId() == m.getId()) count++;
            cout << left << setw(5) << m.getId() << setw(25) << m.getName()
                 << count << "\n";
        }
    }

    // ---- issue / return
    void issueBook() {
        int bid = readInt("Book ID   : ");
        int mid = readInt("Member ID : ");
        Book *b = findBook(bid);
        Member *m = findMember(mid);
        if (!b) { cout << "No such book.\n"; return; }
        if (!m) { cout << "No such member.\n"; return; }
        if (b->isIssued()) { cout << "Book is already issued.\n"; return; }

        time_t now = time(nullptr);
        if (!db.issueBook(bid, mid, now)) {
            cout << "Book is already issued.\n";
            return;
        }
        b->issueTo(mid, now);
        cout << "Issued '" << b->getTitle() << "' to " << m->getName()
             << ".\nDue date: " << formatDate(now + LOAN_DAYS * 24 * 3600)
             << "\n";
    }

    void returnBook() {
        int bid = readInt("Book ID to return: ");
        Book *b = findBook(bid);
        if (!b) { cout << "No such book.\n"; return; }
        if (!b->isIssued()) { cout << "That book was not issued.\n"; return; }

        double days = difftime(time(nullptr), b->getIssueTime()) / (24 * 3600);
        int daysHeld = static_cast<int>(days);
        int late = max(0, daysHeld - LOAN_DAYS);

        if (!db.returnBook(bid)) {
            cout << "That book was not issued.\n";
            return;
        }

        cout << "Returned '" << b->getTitle() << "' (held for " << daysHeld
             << " day(s)).\n";
        if (late > 0)
            cout << "Late by " << late << " day(s). Fine: Rs "
                 << late * FINE_PER_DAY << "\n";
        else
            cout << "Returned on time. No fine.\n";

        b->giveBack();
    }
};

// ------------------------------------------------------------------ menu

void showMenu() {
    cout << "\n===== LIBRARY MANAGEMENT SYSTEM =====\n"
         << " 1. Add book\n"
         << " 2. Remove book\n"
         << " 3. List all books\n"
         << " 4. Search books\n"
         << " 5. Register member\n"
         << " 6. List members\n"
         << " 7. Issue book\n"
         << " 8. Return book\n"
         << " 0. Exit\n"
         << "======================================\n";
}

int main() {
    Library lib;
    try {
        lib.open();
    } catch (const exception &e) {
        cerr << e.what() << "\n";
        return 1;
    }

    while (true) {
        showMenu();
        int choice = readInt("Enter choice: ");
        cout << "\n";
        try {
            lib.load();  // pick up any changes made from the web page
            switch (choice) {
                case 1: lib.addBook(); break;
                case 2: lib.removeBook(); break;
                case 3: lib.listBooks(); break;
                case 4: lib.searchBooks(); break;
                case 5: lib.addMember(); break;
                case 6: lib.listMembers(); break;
                case 7: lib.issueBook(); break;
                case 8: lib.returnBook(); break;
                case 0:
                    cout << "Data saved. Goodbye!\n";
                    return 0;
                default: cout << "Invalid choice.\n";
            }
        } catch (const exception &e) {
            cout << "Database error: " << e.what() << "\n";
        }
    }
}
