// bcrypt 与文件数据层密码加密的自测程序（不依赖图形界面）。
//
// 编译与运行（项目根目录，MinGW g++）：
//   g++ -std=c++17 -O2 -I. tests/password_encryption_test.cpp
//       datastruct.cpp password_hasher.cpp -lbcrypt -o build/password_test.exe
//   ./build/password_test.exe
#include "datastruct.h"
#include "password_hasher.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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

bool readStoredPin(const std::filesystem::path& directory, std::string& out) {
    std::ifstream input(directory / "accounts.dat", std::ios::binary);
    std::string line;
    if (!input || !std::getline(input, line)) return false;
    const std::size_t first = line.find('|');
    const std::size_t second = line.find('|', first + 1);
    const std::size_t third = line.find('|', second + 1);
    if (first == std::string::npos || second == std::string::npos || third == std::string::npos) {
        return false;
    }
    out = line.substr(second + 1, third - second - 1);
    return true;
}

void writeLegacyFiles(const std::filesystem::path& directory, bool withStatements) {
    std::filesystem::create_directories(directory);
    std::ofstream accounts(directory / "accounts.dat", std::ios::binary);
    accounts << "100001|张三|123456|13800000001|50000|0\n"
             << "100002|李四|654321|13800000002|30000|0\n";
    if (!withStatements) return;
    std::ofstream statements(directory / "statements.dat", std::ios::binary);
    statements << "1|100002|2026-11-10 16:12:34|3|50|100001\n"
               << "2|100001|2026-11-10 16:12:34|4|50|100002\n"
               << "3|100001|2026-11-10 16:13:06|5|0|\n";
}

void testBcryptVectors() {
    std::printf("== bcrypt 标准参考向量 ==\n");
    for (const auto& vector : kReferenceVectors) {
        check(bcrypt::hashPasswordWithSalt(vector.password, vector.salt) == vector.expected,
              std::string(vector.password) + " @ " + vector.salt);
    }
}

void testBcryptApi() {
    std::printf("== bcrypt 接口行为 ==\n");
    const std::string first = bcrypt::hashPassword("123456");
    const std::string second = bcrypt::hashPassword("123456");
    check(first.size() == 60 && first.rfind("$2b$10$", 0) == 0, "哈希格式正确");
    check(bcrypt::isBcryptHash(first), "识别合法哈希");
    check(first != second, "随机盐使同一密码产生不同哈希");
    check(bcrypt::verifyPassword("123456", first), "正确密码校验通过");
    check(!bcrypt::verifyPassword("123457", first), "错误密码被拒绝");
    check(!bcrypt::isBcryptHash("123456"), "明文不会被误判为哈希");
}

void testFileStoreEncryption() {
    std::printf("== 文件数据层：生成与迁移均不保存明文密码 ==\n");
    const std::filesystem::path generated = "test_data_encryption";
    const std::filesystem::path legacy = "test_data_legacy";
    const std::filesystem::path migration = "test_data_migration_guard";
    std::error_code filesystemError;
    std::filesystem::remove_all(generated, filesystemError);
    std::filesystem::remove_all(legacy, filesystemError);
    std::filesystem::remove_all(migration, filesystemError);

    {
        DataStore store(generated.string());
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
              "样本账户可用初始密码校验");
    }
    std::string storedPin;
    check(readStoredPin(generated, storedPin), "读取 accounts.dat 密码字段");
    check(bcrypt::isBcryptHash(storedPin), "accounts.dat 中只保存 bcrypt 哈希");

    writeLegacyFiles(legacy, false);
    {
        DataStore store(legacy.string());
        std::string error;
        check(store.load(error), "旧文件 load() 成功 " + error);
        const Account* account = store.findAccount("100001");
        check(account != nullptr && bcrypt::verifyPassword("123456", account->pinHash),
              "旧文件明文密码迁移后仍能校验");
    }
    check(readStoredPin(legacy, storedPin), "读取迁移后的密码字段");
    check(bcrypt::isBcryptHash(storedPin), "迁移后的 accounts.dat 不含明文密码");

    writeLegacyFiles(migration, true);
    {
        DataStore store(migration.string());
        std::string error;
        check(store.load(error), "带流水的旧文件 load() 成功 " + error);
        check(store.statements.size() == 3, "密码迁移不会丢失 statements.dat 流水");
    }

    std::filesystem::remove_all(generated, filesystemError);
    std::filesystem::remove_all(legacy, filesystemError);
    std::filesystem::remove_all(migration, filesystemError);
}

}  // namespace

int main() {
    testBcryptVectors();
    testBcryptApi();
    testFileStoreEncryption();

    if (failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d FAILURE(S)\n", failures);
    return 1;
}
