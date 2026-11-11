#include "datastruct.h"

#include "password_hasher.h"
#include "sqlite3.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

// 把 UTF-8 目录/文件名转换成文件系统路径。
// Windows 上不能直接用 std::filesystem::path(std::string)（按本地代码页解释）
// 或 u8path（MinGW 的 libstdc++ 在非 UTF-8 locale 下同样会抛
// "Illegal byte sequence" 异常），因此统一手动做 UTF-8 → UTF-16 → path。
std::filesystem::path pathFromUtf8(const std::string& utf8Text) {
#if defined(_WIN32)
    if (utf8Text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(), -1, nullptr, 0);
    if (length <= 0) return std::filesystem::path(utf8Text);
    std::wstring wide(static_cast<std::size_t>(length) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(), -1, wide.data(), length);
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(utf8Text);
#endif
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
    const std::filesystem::path databasePath = pathFromUtf8(baseDirectory) / "atm.db";
    int result = SQLITE_ERROR;
#if defined(_WIN32)
    // Windows 从 SQLite 3.28.11 起会把 UTF-8 文件名解释为 UTF-8，但这里统一走
    // 宽字符接口，避免任何代码页相关的路径问题（含中文目录名）。
    const std::wstring widePath = databasePath.wstring();
    result = sqlite3_open16(widePath.c_str(), &database);
#else
    result = sqlite3_open_v2(databasePath.u8string().c_str(), &database,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
#endif
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
    // 第 3 列 pin 存放的是 bcrypt 哈希，Account::pinHash 直接承接。
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

// 以二进制方式读取整个文本文件。Windows 上使用 _wfopen，保证含中文等
// 非 ASCII 字符的目录名也能正常打开（std::ifstream 的窄字符路径会按本地
// 代码页解释而失败）。
bool readWholeTextFile(const std::filesystem::path& filePath, std::string& content) {
    content.clear();
#ifdef _WIN32
    FILE* file = _wfopen(filePath.c_str(), L"rb");
#else
    FILE* file = std::fopen(filePath.string().c_str(), "rb");
#endif
    if (file == nullptr) return false;
    char buffer[4096];
    std::size_t readBytes = 0;
    while ((readBytes = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        content.append(buffer, readBytes);
    }
    std::fclose(file);
    return true;
}

bool DataStore::loadLegacyAccounts(std::string& error) {
    const std::filesystem::path filePath = pathFromUtf8(baseDirectory) / "accounts.dat";
    std::string fileContent;
    if (!readWholeTextFile(filePath, fileContent)) return true;  // 没有旧文件是正常情况
    std::istringstream input(fileContent);

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
    const std::filesystem::path filePath = pathFromUtf8(baseDirectory) / "statements.dat";
    std::string fileContent;
    if (!readWholeTextFile(filePath, fileContent)) return true;  // 没有旧文件是正常情况
    std::istringstream input(fileContent);

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
    const std::filesystem::path directoryPath = pathFromUtf8(baseDirectory);
    // 只在目录不存在时创建（MinGW 的 create_directories 在目录已存在时可能
    // 也返回错误码，这里避开这种实现差异）。
    if (!std::filesystem::exists(directoryPath, filesystemError)) {
        std::filesystem::create_directories(directoryPath, filesystemError);
    }
    if (filesystemError) {
        error = "无法创建数据库目录。";
        return false;
    }
    if (!openDatabase(error) || !ensureSchema(error)) return false;
    if (!loadAccounts(error)) return false;

    if (accounts.empty()) {
        const bool hasLegacyAccounts = std::filesystem::exists(directoryPath / "accounts.dat");
        if (hasLegacyAccounts) {
            if (!loadLegacyAccounts(error) || !loadLegacyStatements(error)) return false;
            if (accounts.empty()) return createDefaultData(error);
            if (!save(error)) return false;
            // 迁移成功后归档旧文件，避免下次启动重复导入。
            archiveLegacyFile(directoryPath / "accounts.dat");
            archiveLegacyFile(directoryPath / "statements.dat");
            return true;
        }
        return createDefaultData(error);
    }

    // 旧版本数据库里可能残留明文密码：统一升级成 bcrypt 哈希。
    // 关键顺序：必须先 loadStatements() 把流水读进内存，再执行迁移回写。
    // save() 的实现是“清空两张表后按内存缓存重插”，若流水还没加载就回写，
    // 会把 statements 表清空（历史版本踩过这个坑，务必保持先读后写）。
    if (!loadStatements(error)) return false;
    bool upgradedPlaintext = false;
    if (!hashPlaintextPins(upgradedPlaintext)) {
        error = "旧数据中的密码无法加密，已中止加载。";
        return false;
    }
    if (upgradedPlaintext && !save(error)) return false;
    return true;
}

// 把内存缓存中仍是明文的密码就地替换为 bcrypt 哈希。
// 返回 true 表示“可以安全写库”（没有明文，或明文已全部散列成功）；
// 返回 false 表示散列失败（随机数不可用等），调用方必须中止写库。
// upgraded 通过出参报告本次是否真的发生了明文→哈希的升级。
bool DataStore::hashPlaintextPins(bool& upgraded) {
    upgraded = false;
    for (auto& [id, account] : accounts) {
        // 空密码不是“明文秘密”，保持原样（登录校验自然失败）。
        if (account.pinHash.empty()) continue;
        if (bcrypt::isBcryptHash(account.pinHash)) continue;
        const std::string hash = bcrypt::hashPassword(account.pinHash);
        if (hash.empty()) return false;
        account.pinHash = hash;
        upgraded = true;
    }
    return true;
}

bool DataStore::save(std::string& error) const {
    // 最后一道保险：写库前把仍是明文的密码替换成 bcrypt 哈希。
    // 正常情况下 load()/开户/改密流程已经完成散列，这里只兜底；accounts 是
    // 本对象的缓存，逻辑上允许整理，故用 const_cast 完成就地规范化。
    DataStore& self = const_cast<DataStore&>(*this);
    bool upgraded = false;
    if (!self.hashPlaintextPins(upgraded)) {
        error = "密码加密失败，已拒绝写入数据库。";
        return false;
    }
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
        sqlite3_bind_text(accountStatement, 3, account.pinHash.c_str(), -1, SQLITE_TRANSIENT);
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
