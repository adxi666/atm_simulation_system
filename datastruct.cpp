#include "datastruct.h"

#include "sqlite3.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace {
// 旧版本 .dat 文件使用 | 分隔，迁移时仍保留这个解析器。
std::vector<std::string> split(const std::string& line) {
    std::string normalizedLine = line;
    if (!normalizedLine.empty() && normalizedLine.back() == '\r') normalizedLine.pop_back();
    std::vector<std::string> fields;
    std::stringstream stream(normalizedLine);
    std::string field;
    while (std::getline(stream, field, '|')) fields.push_back(field);
    if (!normalizedLine.empty() && normalizedLine.back() == '|') fields.emplace_back();
    return fields;
}

bool parseLongLong(const std::string& value, long long& result) {
    try {
        std::size_t used = 0;
        result = std::stoll(value, &used);
        return used == value.size();
    } catch (...) {
        return false;
    }
}

std::string textColumn(sqlite3_stmt* statement, int index) {
    const unsigned char* value = sqlite3_column_text(statement, index);
    return value == nullptr ? std::string() : reinterpret_cast<const char*>(value);
}

void archiveLegacyFile(const std::filesystem::path& filePath) {
    if (!std::filesystem::exists(filePath)) return;
    std::filesystem::path backupPath = filePath;
    backupPath += ".legacy";
    std::error_code filesystemError;
    if (std::filesystem::exists(backupPath)) {
        std::filesystem::remove(filePath, filesystemError);
    } else {
        std::filesystem::rename(filePath, backupPath, filesystemError);
    }
}

bool execute(sqlite3* database, const char* sql, std::string& error) {
    char* sqliteError = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &sqliteError);
    if (result == SQLITE_OK) return true;
    error = sqliteError == nullptr ? sqlite3_errmsg(database) : sqliteError;
    if (sqliteError != nullptr) sqlite3_free(sqliteError);
    return false;
}

std::vector<Account> sampleAccounts() {
    // 首次运行时提供 20 个案例账户，方便直接演示登录和转账。
    return {
        {"100001", "张三", "123456", "13800000001", 50000, CardStatus::Normal},
        {"100002", "李四", "654321", "13800000002", 30000, CardStatus::Normal},
        {"100003", "王五", "123456", "13800000003", 28000, CardStatus::Normal},
        {"100004", "赵六", "123456", "13800000004", 12000, CardStatus::Normal},
        {"100005", "陈晨", "123456", "13800000005", 65000, CardStatus::Normal},
        {"100006", "刘洋", "123456", "13800000006", 18000, CardStatus::Normal},
        {"100007", "杨帆", "123456", "13800000007", 42000, CardStatus::Normal},
        {"100008", "黄敏", "123456", "13800000008", 36000, CardStatus::Normal},
        {"100009", "周杰", "123456", "13800000009", 9000, CardStatus::Normal},
        {"100010", "吴倩", "123456", "13800000010", 51000, CardStatus::Normal},
        {"100011", "徐宁", "123456", "13800000011", 24000, CardStatus::Normal},
        {"100012", "孙伟", "123456", "13800000012", 47000, CardStatus::Normal},
        {"100013", "胡静", "123456", "13800000013", 15000, CardStatus::Normal},
        {"100014", "朱涛", "123456", "13800000014", 33000, CardStatus::Normal},
        {"100015", "高磊", "123456", "13800000015", 27000, CardStatus::Normal},
        {"100016", "林芳", "123456", "13800000016", 58000, CardStatus::Normal},
        {"100017", "郑凯", "123456", "13800000017", 21000, CardStatus::Normal},
        {"100018", "马超", "123456", "13800000018", 39000, CardStatus::Normal},
        {"100019", "何琳", "123456", "13800000019", 44000, CardStatus::Normal},
        {"100020", "罗强", "123456", "13800000020", 31000, CardStatus::Normal}
    };
}
}

DataStore::DataStore(std::string directory) : baseDirectory(std::move(directory)) {}

DataStore::~DataStore() {
    if (database != nullptr) sqlite3_close(database);
}

void DataStore::setBaseDirectory(std::string directory) {
    if (database != nullptr) {
        sqlite3_close(database);
        database = nullptr;
    }
    baseDirectory = std::move(directory);
}

bool DataStore::openDatabase(std::string& error) {
    if (database != nullptr) return true;
    const std::filesystem::path databasePath = std::filesystem::path(baseDirectory) / "atm.db";
    const int result = sqlite3_open_v2(databasePath.u8string().c_str(), &database,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (result != SQLITE_OK) {
        error = database == nullptr ? "无法打开 SQLite 数据库。" : sqlite3_errmsg(database);
        if (database != nullptr) sqlite3_close(database);
        database = nullptr;
        return false;
    }
    return execute(database, "PRAGMA foreign_keys = ON;", error);
}

bool DataStore::ensureSchema(std::string& error) const {
    static constexpr char schema[] =
        "CREATE TABLE IF NOT EXISTS accounts ("
        "id TEXT PRIMARY KEY NOT NULL,"
        "name TEXT NOT NULL,"
        "pin TEXT NOT NULL,"
        "phone TEXT NOT NULL,"
        "balance_cents INTEGER NOT NULL DEFAULT 0,"
        "status INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS statements ("
        "id INTEGER PRIMARY KEY NOT NULL,"
        "account_id TEXT NOT NULL,"
        "time TEXT NOT NULL,"
        "type INTEGER NOT NULL,"
        "amount_cents INTEGER NOT NULL DEFAULT 0,"
        "to_account TEXT NOT NULL DEFAULT '',"
        "FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE"
        ");";
    return execute(database, schema, error);
}

bool DataStore::loadAccounts(std::string& error) {
    accounts.clear();
    sqlite3_stmt* statement = nullptr;
    const char* sql = "SELECT id, name, pin, phone, balance_cents, status FROM accounts ORDER BY id;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        return false;
    }

    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        const long long balance = sqlite3_column_int64(statement, 4);
        const int status = sqlite3_column_int(statement, 5);
        if (status != 0 && status != 1) {
            error = "数据库中的账户状态无效。";
            sqlite3_finalize(statement);
            return false;
        }
        const std::string id = textColumn(statement, 0);
        accounts[id] = Account{
            id, textColumn(statement, 1), textColumn(statement, 2), textColumn(statement, 3),
            balance, status == 0 ? CardStatus::Normal : CardStatus::Retained
        };
    }
    if (result != SQLITE_DONE) {
        error = sqlite3_errmsg(database);
        sqlite3_finalize(statement);
        return false;
    }
    sqlite3_finalize(statement);
    return true;
}

bool DataStore::loadStatements(std::string& error) {
    statements.clear();
    sqlite3_stmt* statement = nullptr;
    const char* sql = "SELECT id, account_id, time, type, amount_cents, to_account "
                      "FROM statements ORDER BY id;";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        return false;
    }

    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        const int type = sqlite3_column_int(statement, 3);
        if (type < 1 || type > 5) {
            error = "数据库中的流水类型无效。";
            sqlite3_finalize(statement);
            return false;
        }
        statements.push_back(Statement{
            sqlite3_column_int64(statement, 0), textColumn(statement, 1), textColumn(statement, 2),
            static_cast<StatementType>(type), sqlite3_column_int64(statement, 4), textColumn(statement, 5)
        });
    }
    if (result != SQLITE_DONE) {
        error = sqlite3_errmsg(database);
        sqlite3_finalize(statement);
        return false;
    }
    sqlite3_finalize(statement);
    return true;
}

bool DataStore::loadLegacyAccounts(std::string& error) {
    const std::filesystem::path filePath = std::filesystem::path(baseDirectory) / "accounts.dat";
    std::ifstream input(filePath, std::ios::binary);
    if (!input) return true;

    accounts.clear();
    bool sawLegacyFormat = false;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split(line);
        if (fields.size() != 5 && fields.size() != 6) {
            error = "accounts.dat 文件格式错误。";
            return false;
        }

        const bool legacyFormat = fields.size() == 5;
        sawLegacyFormat = sawLegacyFormat || legacyFormat;
        const std::size_t balanceIndex = legacyFormat ? 3 : 4;
        const std::size_t statusIndex = legacyFormat ? 4 : 5;
        long long balance = 0;
        long long status = 0;
        if (!parseLongLong(fields[balanceIndex], balance) || !parseLongLong(fields[statusIndex], status) ||
            (status != 0 && status != 1) || fields[0].empty() || fields[2].empty()) {
            error = "accounts.dat 文件中存在无效账户数据。";
            return false;
        }
        const std::string phone = legacyFormat ? "13800000000" : fields[3];
        accounts[fields[0]] = Account{
            fields[0], fields[1], fields[2], phone, balance,
            status == 0 ? CardStatus::Normal : CardStatus::Retained
        };
    }

    if (sawLegacyFormat && accounts.size() < 20) {
        for (const auto& sample : sampleAccounts()) {
            if (accounts.find(sample.id) == accounts.end()) accounts.emplace(sample.id, sample);
        }
    }
    return true;
}

bool DataStore::loadLegacyStatements(std::string& error) {
    const std::filesystem::path filePath = std::filesystem::path(baseDirectory) / "statements.dat";
    std::ifstream input(filePath, std::ios::binary);
    if (!input) return true;

    statements.clear();
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split(line);
        if (fields.size() != 6) {
            error = "statements.dat 文件格式错误。";
            return false;
        }

        long long id = 0;
        long long type = 0;
        long long amount = 0;
        if (!parseLongLong(fields[0], id) || !parseLongLong(fields[3], type) ||
            !parseLongLong(fields[4], amount) || type < 1 || type > 5) {
            error = "statements.dat 文件中存在无效流水数据。";
            return false;
        }
        statements.push_back(Statement{
            id, fields[1], fields[2], static_cast<StatementType>(type), amount, fields[5]
        });
    }
    return true;
}

bool DataStore::load(std::string& error) {
    std::error_code filesystemError;
    std::filesystem::create_directories(baseDirectory, filesystemError);
    if (filesystemError) {
        error = "无法创建数据库目录。";
        return false;
    }
    if (!openDatabase(error) || !ensureSchema(error)) return false;
    if (!loadAccounts(error)) return false;

    if (accounts.empty()) {
        const bool hasLegacyAccounts = std::filesystem::exists(
            std::filesystem::path(baseDirectory) / "accounts.dat");
        if (hasLegacyAccounts) {
            if (!loadLegacyAccounts(error) || !loadLegacyStatements(error)) return false;
            if (accounts.empty()) return createDefaultData(error);
            if (!save(error)) return false;
            // 迁移成功后归档旧文件，避免下次启动重复导入。
            archiveLegacyFile(std::filesystem::path(baseDirectory) / "accounts.dat");
            archiveLegacyFile(std::filesystem::path(baseDirectory) / "statements.dat");
            return true;
        }
        return createDefaultData(error);
    }
    return loadStatements(error);
}

bool DataStore::save(std::string& error) const {
    if (database == nullptr) {
        error = "SQLite 数据库尚未打开。";
        return false;
    }
    if (!execute(database, "BEGIN IMMEDIATE TRANSACTION;", error)) return false;

    auto rollback = [&]() {
        std::string ignored;
        execute(database, "ROLLBACK;", ignored);
    };

    if (!execute(database, "DELETE FROM statements; DELETE FROM accounts;", error)) {
        rollback();
        return false;
    }

    sqlite3_stmt* accountStatement = nullptr;
    const char* accountSql = "INSERT INTO accounts "
                             "(id, name, pin, phone, balance_cents, status) VALUES (?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(database, accountSql, -1, &accountStatement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        rollback();
        return false;
    }
    for (const auto& [id, account] : accounts) {
        sqlite3_bind_text(accountStatement, 1, account.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(accountStatement, 2, account.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(accountStatement, 3, account.pin.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(accountStatement, 4, account.phone.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(accountStatement, 5, account.balanceCents);
        sqlite3_bind_int(accountStatement, 6, account.status == CardStatus::Normal ? 0 : 1);
        if (sqlite3_step(accountStatement) != SQLITE_DONE) {
            error = sqlite3_errmsg(database);
            sqlite3_finalize(accountStatement);
            rollback();
            return false;
        }
        sqlite3_reset(accountStatement);
        sqlite3_clear_bindings(accountStatement);
    }
    sqlite3_finalize(accountStatement);

    sqlite3_stmt* statementStatement = nullptr;
    const char* statementSql = "INSERT INTO statements "
                               "(id, account_id, time, type, amount_cents, to_account) "
                               "VALUES (?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(database, statementSql, -1, &statementStatement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(database);
        rollback();
        return false;
    }
    for (const auto& statement : statements) {
        sqlite3_bind_int64(statementStatement, 1, statement.id);
        sqlite3_bind_text(statementStatement, 2, statement.accountId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statementStatement, 3, statement.time.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statementStatement, 4, static_cast<int>(statement.type));
        sqlite3_bind_int64(statementStatement, 5, statement.amountCents);
        sqlite3_bind_text(statementStatement, 6, statement.toAccount.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statementStatement) != SQLITE_DONE) {
            error = sqlite3_errmsg(database);
            sqlite3_finalize(statementStatement);
            rollback();
            return false;
        }
        sqlite3_reset(statementStatement);
        sqlite3_clear_bindings(statementStatement);
    }
    sqlite3_finalize(statementStatement);

    if (!execute(database, "COMMIT;", error)) {
        rollback();
        return false;
    }
    return true;
}

bool DataStore::createDefaultData(std::string& error) {
    accounts.clear();
    statements.clear();
    for (const auto& account : sampleAccounts()) accounts.emplace(account.id, account);
    return save(error);
}

Account* DataStore::findAccount(const std::string& id) {
    const auto it = accounts.find(id);
    return it == accounts.end() ? nullptr : &it->second;
}

const Account* DataStore::findAccount(const std::string& id) const {
    const auto it = accounts.find(id);
    return it == accounts.end() ? nullptr : &it->second;
}

long long DataStore::nextStatementId() const {
    long long next = 1;
    for (const auto& statement : statements) {
        if (statement.id >= next) next = statement.id + 1;
    }
    return next;
}

void DataStore::addStatement(const Statement& statement) {
    statements.push_back(statement);
}

void DataStore::removeLastStatement() {
    if (!statements.empty()) statements.pop_back();
}
