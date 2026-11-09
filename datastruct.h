#ifndef ATM_DATASTRUCT_H
#define ATM_DATASTRUCT_H

#include <map>
#include <string>
#include <vector>

enum class CardStatus {
    Normal = 0,
    Retained = 1
};

enum class StatementType {
    Deposit = 1,
    Withdraw = 2,
    TransferOut = 3,
    TransferIn = 4,
    PasswordChanged = 5
};

struct Account {
    std::string id;
    std::string name;
    std::string pin;
    std::string phone;
    long long balanceCents = 0;
    CardStatus status = CardStatus::Normal;
};

struct Statement {
    long long id = 0;
    std::string accountId;
    std::string time;
    StatementType type = StatementType::Deposit;
    long long amountCents = 0;
    std::string toAccount;
};

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
};

#endif
