#ifndef ATM_DATASTRUCT_H
#define ATM_DATASTRUCT_H

#include <map>
#include <string>
#include <vector>

struct sqlite3;

// 密码存储采用 bcrypt（加盐 + 慢哈希，具体思路见 password_hasher.h 顶部注释）。

// 卡片状态：正常卡可以登录，吞卡后禁止继续使用。
enum class CardStatus {
    Normal = 0,
    Retained = 1
};

// 流水类型，数值会保存到 SQLite 的 type 字段。
enum class StatementType {
    Deposit = 1,
    Withdraw = 2,
    TransferOut = 3,
    TransferIn = 4,
    PasswordChanged = 5
};

// 账户信息。金额统一使用“分”保存，避免浮点数精度误差。
struct Account {
    std::string id;
    std::string name;
    // 密码列（数据库 accounts.pin）。这里保存的是 bcrypt 哈希，例如
    // "$2b$10$N9qo8uLOickgx2ZMRZoMyeIjZAgcfl7p92ldGxad68LJZdL17lhWy"，
    // 绝不是明文密码；校验统一使用 bcrypt::verifyPassword()。
    std::string pinHash;
    std::string phone;
    long long balanceCents = 0;
    CardStatus status = CardStatus::Normal;
};

// 单笔交易流水，accountId 表示这条流水归属的账户。
struct Statement {
    long long id = 0;
    std::string accountId;
    std::string time;
    StatementType type = StatementType::Deposit;
    long long amountCents = 0;
    std::string toAccount;
};

// SQLite 本地数据库仓库，同时保留内存缓存供界面业务逻辑使用。
class DataStore {
public:
    explicit DataStore(std::string baseDirectory = ".");
    ~DataStore();

    DataStore(const DataStore&) = delete;
    DataStore& operator=(const DataStore&) = delete;

    void setBaseDirectory(std::string directory);
    bool load(std::string& error);
    bool save(std::string& error) const;

    Account* findAccount(const std::string& id);
    const Account* findAccount(const std::string& id) const;
    long long nextStatementId() const;
    void addStatement(const Statement& statement);
    void removeLastStatement();

    std::map<std::string, Account> accounts;
    std::vector<Statement> statements;

private:
    std::string baseDirectory;
    sqlite3* database = nullptr;

    bool openDatabase(std::string& error);
    bool ensureSchema(std::string& error) const;
    bool loadAccounts(std::string& error);
    bool loadStatements(std::string& error);
    bool loadLegacyAccounts(std::string& error);
    bool loadLegacyStatements(std::string& error);
    bool createDefaultData(std::string& error);
    // 把仍为明文的密码就地替换为 bcrypt 哈希。
    // 返回 true 表示可以安全落盘；返回 false 表示散列失败（随机数不可用等），
    // 调用方必须中止写库。upgraded 报告本次是否真的发生了明文→哈希升级。
    // 注意：必须在 loadStatements() 之后调用再 save()，否则 save() 的
    // “清空重插”会把尚未加载的流水一并清掉。
    bool hashPlaintextPins(bool& upgraded);
};

#endif
