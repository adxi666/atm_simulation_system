#ifndef ATM_DATASTRUCT_H
#define ATM_DATASTRUCT_H

#include <map>
#include <string>
#include <vector>

// 密码存储采用 bcrypt（加盐 + 慢哈希），持久化文件只保存 pinHash。

// 卡片状态：正常卡可以登录，吞卡后禁止继续使用。
enum class CardStatus {
    Normal = 0,
    Retained = 1
};

// 流水类型，数值会直接写入 statements.dat。
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
    // 这里保存的是 bcrypt 哈希，不是用户输入的明文密码。
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

// 本地文本数据仓库：负责账户和交易流水的加载、保存及查询。
class DataStore {
public:
    explicit DataStore(std::string baseDirectory = ".");

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
    bool loadAccounts(std::string& error);
    bool loadStatements(std::string& error);
    bool createDefaultData(std::string& error);
    // 把旧文件中仍为明文的密码升级为 bcrypt；失败时禁止继续写回文件。
    bool hashPlaintextPins(bool& upgraded);
};

#endif
