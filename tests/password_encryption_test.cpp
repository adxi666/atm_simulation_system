// bcrypt 与数据层密码加密的自测程序（不依赖图形界面）。
//
// 编译与运行（在项目根目录，MinGW g++；也可直接用 CMake 的 BUILD_TESTS 选项）：
//   mkdir -p build
//   g++ -std=c++17 -O2 -I. -Ithird_party/sqlite tests/password_encryption_test.cpp
//       datastruct.cpp password_hasher.cpp third_party/sqlite/sqlite3.c -lbcrypt
//       -o build/password_test.exe
//   ./build/password_test.exe
//
// 参考向量由独立的 bcryptjs（纯 JS 版 bcrypt）生成：固定盐、固定密码，
// 本实现必须与其逐字符一致，用来证明密钥扩展、加密与编码都符合标准 bcrypt。
#include "datastruct.h"
#include "password_hasher.h"
#include "sqlite3.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

struct Vector {
    const char* password;
    const char* salt;
    const char* expected;
};

const std::vector<Vector> kReferenceVectors = {
    {"123456", "$2b$10$abcdefghijklmnopqrstuu",
     "$2b$10$abcdefghijklmnopqrstuuxD0vCwu/f2ahNVJLL35ZYA2xuiRyPH6"},
    {"654321", "$2b$10$abcdefghijklmnopqrstuu",
     "$2b$10$abcdefghijklmnopqrstuut4BIfjsBRgXgoWYJIu7gGbSz9idv7iS"},
    {"correct horse battery", "$2b$12$abcdefghijklmnopqrstuu",
     "$2b$12$abcdefghijklmnopqrstuuE3j5FFjC71WyDL2KIK7ShSoj8tPD3m6"},
    {"123456", "$2a$10$abcdefghijklmnopqrstuu",
     "$2a$10$abcdefghijklmnopqrstuuxD0vCwu/f2ahNVJLL35ZYA2xuiRyPH6"},
    {"000000", "$2b$04$abcdefghijklmnopqrstuu",
     "$2b$04$abcdefghijklmnopqrstuuS4L5df2FgDdIAPtqnZ7TvT.cXakr66S"},
    {"123456", "$2b$04$fxURwUXmeoyn8YDkRTaDvu",
     "$2b$04$fxURwUXmeoyn8YDkRTaDvuRvrXXkVLim6jqkIfSK7ZXHg/kPaXH3y"},
    {"654321", "$2b$05$HKih40q8OnkGA6Yz988WG.",
     "$2b$05$HKih40q8OnkGA6Yz988WG.QSoeagg.VF9Do4M2dbXa3QDAKKftTcG"},
    {"123456", "$2b$06$DhmePAeACVOjIMw8lKFMne",
     "$2b$06$DhmePAeACVOjIMw8lKFMneaVgLk0VnelaDYPsUpAwMJ.jWa4KkiMG"},
    {"888888", "$2b$07$AjK.oE.D43IEwe.c7mLsqe",
     "$2b$07$AjK.oE.D43IEwe.c7mLsqeoFFONsvLT464gEyX4NuNawuXA5BXXOu"},
    {"111111", "$2b$04$Y7ZAae3AfbDTkIqX71pe..",
     "$2b$04$Y7ZAae3AfbDTkIqX71pe..dp3GsjfeAdXy86dwpxwvlgVhROxxMFq"},
    {"admin123", "$2b$05$giTF9mGkUwA2m1CSVRBGSu",
     "$2b$05$giTF9mGkUwA2m1CSVRBGSuCGN6vrPLCURUFWUtLMmcz0hWsBlQ0Cy"},
};

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  [ok] %s\n", what.c_str());
    } else {
        std::printf("  [FAIL] %s\n", what.c_str());
        ++failures;
    }
}

bool readPinColumn(const std::string& databasePath, std::string& out) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(databasePath.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (database != nullptr) sqlite3_close(database);
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(database, "SELECT pin FROM accounts WHERE id='100001';", -1, &statement,
                           nullptr) == SQLITE_OK) {
        if (sqlite3_step(statement) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(statement, 0);
            out = text == nullptr ? std::string() : reinterpret_cast<const char*>(text);
            ok = true;
        }
        sqlite3_finalize(statement);
    }
    sqlite3_close(database);
    return ok;
}

// 指定数据库路径与建表语句，先落一份“旧格式”（明文密码）数据库，用于迁移测试。
void createLegacyDatabase(const std::string& databasePath, const char* accountsSql) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(databasePath.c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (database != nullptr) sqlite3_close(database);
        return;
    }
    char* message = nullptr;
    sqlite3_exec(database,
                 "CREATE TABLE accounts (id TEXT PRIMARY KEY, name TEXT, pin TEXT, phone TEXT,"
                 " balance_cents INTEGER, status INTEGER);"
                 "CREATE TABLE statements (id INTEGER PRIMARY KEY, account_id TEXT, time TEXT,"
                 " type INTEGER, amount_cents INTEGER, to_account TEXT);",
                 nullptr, nullptr, &message);
    sqlite3_free(message);
    sqlite3_exec(database, accountsSql, nullptr, nullptr, &message);
    sqlite3_free(message);
    sqlite3_close(database);
}

void testBcryptVectors() {
    std::printf("== bcrypt 标准参考向量（与 bcryptjs 逐字符比对） ==\n");
    for (const auto& vector : kReferenceVectors) {
        const std::string actual = bcrypt::hashPasswordWithSalt(vector.password, vector.salt);
        const std::string label = std::string(vector.password) + " @ " + vector.salt;
        if (actual == vector.expected) {
            check(true, label);
        } else {
            std::printf("  [FAIL] %s\n      expected %s\n      actual   %s\n", label.c_str(),
                        vector.expected, actual.c_str());
            ++failures;
        }
    }
}

void testBcryptApi() {
    std::printf("== bcrypt 接口行为 ==\n");
    const std::string first = bcrypt::hashPassword("123456");
    const std::string second = bcrypt::hashPassword("123456");
    check(first.size() == 60 && first.rfind("$2b$10$", 0) == 0, "哈希格式为 $2b$10$ + 53 字符");
    check(bcrypt::isBcryptHash(first), "isBcryptHash 识别合法哈希");
    check(first != second, "随机盐使同一密码产生不同哈希");
    check(bcrypt::verifyPassword("123456", first), "正确密码校验通过");
    check(!bcrypt::verifyPassword("123457", first), "错误密码被拒绝");
    check(!bcrypt::verifyPassword("", first), "空密码被拒绝");
    check(!bcrypt::isBcryptHash("123456"), "明文不会被误判为哈希");
    check(!bcrypt::isBcryptHash("$2b$10$short"), "残缺哈希被拒绝");
    check(bcrypt::hashPasswordWithSalt("123456", "$2b$10$!!!!!!!!!!!!!!!!!!!!").empty(),
          "非法盐字符被拒绝");
}

void testDataStoreEncryption() {
    std::printf("== 数据层：新库生成时密码即加密 ==\n");
    const std::string workDir = "test_data_encryption";
    std::error_code filesystemError;
    std::filesystem::remove_all(workDir, filesystemError);
    std::filesystem::create_directories(workDir, filesystemError);

    std::string storedPin;
    {
        DataStore store(workDir);
        std::string error;
        check(store.load(error), "首次 load() 成功 " + error);
        check(store.accounts.size() == 20, "生成 20 个样本账户");

        bool allHashed = true;
        for (const auto& entry : store.accounts) {
            if (!bcrypt::isBcryptHash(entry.second.pinHash)) allHashed = false;
        }
        check(allHashed, "内存缓存中全部为 bcrypt 哈希");

        const Account* account = store.findAccount("100001");
        check(account != nullptr && bcrypt::verifyPassword("123456", account->pinHash),
              "样本账户 100001 可用 123456 校验");
        check(account != nullptr && !bcrypt::verifyPassword("654321", account->pinHash),
              "样本账户 100001 拒绝错误密码");
    }

    check(readPinColumn(workDir + "/atm.db", storedPin), "读取数据库 pin 列");
    check(bcrypt::isBcryptHash(storedPin), "数据库文件中 pin 为 bcrypt 哈希：" + storedPin);

    std::printf("== 数据层：旧库明文密码自动迁移 ==\n");
    const std::string legacyDir = "test_data_legacy";
    std::filesystem::remove_all(legacyDir, filesystemError);
    std::filesystem::create_directories(legacyDir, filesystemError);
    createLegacyDatabase(legacyDir + "/atm.db",
                         "INSERT INTO accounts VALUES ('100001','张三','123456','13800000001',50000,0);"
                         "INSERT INTO accounts VALUES ('100002','李四','654321','13800000002',30000,0);");

    {
        DataStore store(legacyDir);
        std::string error;
        check(store.load(error), "旧库 load() 成功 " + error);
        const Account* account = store.findAccount("100001");
        check(account != nullptr && bcrypt::verifyPassword("123456", account->pinHash),
              "旧库明文密码迁移后仍能校验");
    }

    std::string migratedPin;
    check(readPinColumn(legacyDir + "/atm.db", migratedPin), "读取迁移后的 pin 列");
    check(bcrypt::isBcryptHash(migratedPin), "迁移后数据库中的 pin 已变为哈希：" + migratedPin);

    std::filesystem::remove_all(workDir, filesystemError);
    std::filesystem::remove_all(legacyDir, filesystemError);
}

// 回归测试：旧库迁移（明文→哈希）时，save() 会把“内存中的 account/statement
// 全量重写”。历史上曾因先 save 后 loadStatements 导致流水被清空，这里固化守卫。
void testMigrationPreservesStatements() {
    std::printf("== 回归测试：迁移不丢流水 ==\n");
    const std::string directory = "test_data_migration_guard";
    std::error_code filesystemError;
    std::filesystem::remove_all(directory, filesystemError);
    std::filesystem::create_directories(directory, filesystemError);

    sqlite3* database = nullptr;
    check(sqlite3_open_v2((directory + "/atm.db").c_str(), &database,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK,
          "创建旧格式数据库");
    char* message = nullptr;
    sqlite3_exec(database,
                 "CREATE TABLE accounts (id TEXT PRIMARY KEY, name TEXT, pin TEXT, phone TEXT,"
                 " balance_cents INTEGER, status INTEGER);"
                 "CREATE TABLE statements (id INTEGER PRIMARY KEY, account_id TEXT, time TEXT,"
                 " type INTEGER, amount_cents INTEGER, to_account TEXT);"
                 "INSERT INTO accounts VALUES ('100001','张三','123456','13800000001',50000,0);"
                 "INSERT INTO accounts VALUES ('100002','李四','654321','13800000002',30000,0);"
                 "INSERT INTO statements VALUES (1,'100002','2026-11-10 16:12:34',3,50,'100001');"
                 "INSERT INTO statements VALUES (2,'100001','2026-11-10 16:12:34',4,50,'100002');"
                 "INSERT INTO statements VALUES (3,'100001','2026-11-10 16:13:06',5,0,'');",
                 nullptr, nullptr, &message);
    sqlite3_free(message);
    sqlite3_close(database);

    {
        DataStore store(directory);
        std::string error;
        check(store.load(error), "旧库 load() 成功 " + error);
        check(store.statements.size() == 3, "迁移后流水仍为 3 条（没有被洗掉）");
        const Account* account = store.findAccount("100001");
        check(account != nullptr && bcrypt::isBcryptHash(account->pinHash), "迁移后密码已是哈希");
    }

    std::filesystem::remove_all(directory, filesystemError);
}

}  // namespace

int main() {
    testBcryptVectors();
    testBcryptApi();
    testDataStoreEncryption();
    testMigrationPreservesStatements();

    if (failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d FAILURE(S)\n", failures);
    return 1;
}
