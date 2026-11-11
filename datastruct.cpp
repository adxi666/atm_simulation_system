#include "datastruct.h"

#include "password_hasher.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace {
// ATM 数据文件采用 | 分隔的 UTF-8 文本格式，密码字段保存 bcrypt 哈希。
std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, '|')) fields.push_back(field);
    if (!line.empty() && line.back() == '|') fields.emplace_back();
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

std::vector<Account> sampleAccounts() {
    // 首次运行时提供 20 个案例账户，明文初始密码只在创建哈希时短暂存在内存中。
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

bool DataStore::hashPlaintextPins(bool& upgraded) {
    upgraded = false;
    for (auto& [id, account] : accounts) {
        if (account.pinHash.empty() || bcrypt::isBcryptHash(account.pinHash)) continue;
        const std::string hash = bcrypt::hashPassword(account.pinHash);
        if (hash.empty()) return false;
        account.pinHash = hash;
        upgraded = true;
    }
    return true;
}

bool DataStore::createDefaultData(std::string& error) {
    accounts.clear();
    statements.clear();
    for (auto account : sampleAccounts()) {
        account.pinHash = bcrypt::hashPassword(account.pinHash);
        if (account.pinHash.empty()) {
            error = "默认账户密码加密失败。";
            return false;
        }
        accounts.emplace(account.id, std::move(account));
    }
    return save(error);
}

bool DataStore::loadAccounts(std::string& error) {
    std::ifstream input(std::filesystem::path(baseDirectory) / "accounts.dat", std::ios::binary);
    if (!input) return createDefaultData(error);

    accounts.clear();
    std::string line;
    // 兼容旧版五字段账户文件，并在加载时补充手机号和缺失的样本账户。
    bool sawLegacyFormat = false;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split(line);
        if (fields.size() != 5 && fields.size() != 6) {
            error = "accounts.dat 文件格式错误。";
            return false;
        }

        long long balance = 0;
        long long status = 0;
        const bool legacyFormat = fields.size() == 5;
        sawLegacyFormat = sawLegacyFormat || legacyFormat;
        const std::string phone = legacyFormat ? "13800000000" : fields[3];
        const std::size_t balanceIndex = legacyFormat ? 3 : 4;
        const std::size_t statusIndex = legacyFormat ? 4 : 5;
        if (!parseLongLong(fields[balanceIndex], balance) || !parseLongLong(fields[statusIndex], status) ||
            (status != 0 && status != 1) || fields[0].empty() || fields[2].empty()) {
            error = "accounts.dat 文件中存在无效账户数据。";
            return false;
        }
        accounts[fields[0]] = Account{
            fields[0], fields[1], fields[2], phone, balance,
            status == 0 ? CardStatus::Normal : CardStatus::Retained
        };
    }

    if (accounts.empty()) {
        error = "accounts.dat 中没有账户数据。";
        return false;
    }
    if (sawLegacyFormat && accounts.size() < 20) {
        for (const auto& sample : sampleAccounts()) {
            if (accounts.find(sample.id) == accounts.end()) accounts.emplace(sample.id, sample);
        }
    }
    return true;
}

bool DataStore::loadStatements(std::string& error) {
    std::ifstream input(std::filesystem::path(baseDirectory) / "statements.dat", std::ios::binary);
    if (!input) {
        statements.clear();
        return true;
    }

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
        error = "无法创建数据目录。";
        return false;
    }
    if (!loadAccounts(error) || !loadStatements(error)) return false;

    // 旧版本文件可能保存了明文密码；先完整加载账户和流水，再一次性安全迁移。
    bool upgraded = false;
    if (!hashPlaintextPins(upgraded)) {
        error = "旧数据中的密码无法加密，已中止加载。";
        return false;
    }
    if (upgraded && !save(error)) return false;
    return true;
}

bool DataStore::save(std::string& error) const {
    std::error_code filesystemError;
    std::filesystem::create_directories(baseDirectory, filesystemError);
    if (filesystemError) {
        error = "无法创建数据目录。";
        return false;
    }

    // 最后一道保险：任何非 bcrypt 值都不得写入持久化文件。
    for (const auto& [id, account] : accounts) {
        if (!bcrypt::isBcryptHash(account.pinHash)) {
            error = "检测到未加密密码，已拒绝写入 accounts.dat。";
            return false;
        }
    }

    // 账户和流水分别保存，便于课程作业直接检查文件内容。
    std::ofstream accountOutput(std::filesystem::path(baseDirectory) / "accounts.dat",
                                std::ios::binary | std::ios::trunc);
    if (!accountOutput) {
        error = "无法写入 accounts.dat。";
        return false;
    }
    for (const auto& [id, account] : accounts) {
        accountOutput << account.id << '|' << account.name << '|' << account.pinHash << '|'
                      << account.phone << '|' << account.balanceCents << '|'
                      << (account.status == CardStatus::Normal ? 0 : 1) << '\n';
    }
    accountOutput.flush();
    if (!accountOutput) {
        error = "保存 accounts.dat 时发生写入错误。";
        return false;
    }

    std::ofstream statementOutput(std::filesystem::path(baseDirectory) / "statements.dat",
                                  std::ios::binary | std::ios::trunc);
    if (!statementOutput) {
        error = "无法写入 statements.dat。";
        return false;
    }
    for (const auto& statement : statements) {
        statementOutput << statement.id << '|' << statement.accountId << '|' << statement.time << '|'
                        << static_cast<int>(statement.type) << '|' << statement.amountCents << '|'
                        << statement.toAccount << '\n';
    }
    statementOutput.flush();
    if (!statementOutput) {
        error = "保存 statements.dat 时发生写入错误。";
        return false;
    }
    return true;
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
