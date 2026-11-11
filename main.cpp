#include "datastruct.h"
#include "password_hasher.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cwctype>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr wchar_t WINDOW_CLASS_NAME[] = L"AtmSimulationWindow";
constexpr wchar_t WINDOW_TITLE[] = L"ATM 仿真系统";

enum ControlId {
    IDC_LOGIN_ID = 1001,
    IDC_LOGIN_PIN,
    IDC_LOGIN_BUTTON,
    IDC_FORGOT_PASSWORD_BUTTON,
    IDC_OPEN_ACCOUNT_BUTTON,
    IDC_OPEN_ID,
    IDC_OPEN_NAME,
    IDC_OPEN_PHONE,
    IDC_OPEN_PIN,
    IDC_OPEN_CONFIRM_PIN,
    IDC_OPEN_BUTTON,
    IDC_RESET_ID,
    IDC_RESET_PHONE,
    IDC_RESET_NEW_PIN,
    IDC_RESET_CONFIRM_PIN,
    IDC_RESET_BUTTON,
    IDC_DEPOSIT_AMOUNT,
    IDC_DEPOSIT_BUTTON,
    IDC_WITHDRAW_AMOUNT,
    IDC_WITHDRAW_BUTTON,
    IDC_TRANSFER_TARGET,
    IDC_TRANSFER_AMOUNT,
    IDC_TRANSFER_BUTTON,
    IDC_OLD_PIN,
    IDC_NEW_PIN,
    IDC_CONFIRM_PIN,
    IDC_CHANGE_PIN_BUTTON,
    IDC_DELETE_PASSWORD,
    IDC_DELETE_BUTTON,
    IDC_BACK_BUTTON,
    IDM_BALANCE = 2001,
    IDM_DEPOSIT,
    IDM_WITHDRAW,
    IDM_TRANSFER,
    IDM_STATEMENTS,
    IDM_CHANGE_PIN,
    IDM_DELETE_ACCOUNT,
    IDM_LOGOUT,
    IDM_EXIT
};

enum class Page {
    Login,
    OpenAccount,
    ResetPassword,
    Menu,
    Balance,
    Deposit,
    Withdraw,
    Transfer,
    Statements,
    ChangePin,
    DeleteAccount
};

HWND g_window = nullptr;
HFONT g_font = nullptr;
DataStore g_store(".");
std::string g_currentAccountId;
int g_loginFailures = 0;
Page g_page = Page::Login;
std::vector<HWND> g_pageControls;

// Win32 窗口和本地数据文件之间使用 UTF-8/UTF-16 转换。
std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring readText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

void setFont(HWND control) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
}

HWND addControl(const wchar_t* className, const wchar_t* text, DWORD style,
                int x, int y, int width, int height, int id = 0,
                DWORD exStyle = 0) {
    HWND control = CreateWindowExW(exStyle, className, text, WS_CHILD | WS_VISIBLE | style,
                                  x, y, width, height, g_window,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr);
    if (control) {
        setFont(control);
        g_pageControls.push_back(control);
    }
    return control;
}

HWND addLabel(const std::wstring& text, int x, int y, int width = 700, int height = 32) {
    return addControl(L"STATIC", text.c_str(), SS_LEFT, x, y, width, height);
}

HWND addButton(const std::wstring& text, int id, int x, int y, int width = 180, int height = 42) {
    return addControl(L"BUTTON", text.c_str(), BS_PUSHBUTTON, x, y, width, height, id);
}

HWND addEdit(const std::wstring& text, int id, int x, int y, int width = 300, int height = 34,
             bool password = false) {
    DWORD style = WS_BORDER | ES_AUTOHSCROLL;
    if (password) style |= ES_PASSWORD;
    return addControl(L"EDIT", text.c_str(), style, x, y, width, height, id, WS_EX_CLIENTEDGE);
}

std::wstring formatMoney(long long cents) {
    std::wstringstream stream;
    stream << L"¥" << (cents / 100) << L"." << std::setw(2)
           << std::setfill(L'0') << std::llabs(cents % 100);
    return stream.str();
}

// 只接受正数金额，并统一转换成“分”保存。
bool parseAmount(const std::wstring& input, long long& cents) {
    if (input.empty()) return false;
    const std::size_t dot = input.find(L'.');
    if (dot != std::wstring::npos && input.find(L'.', dot + 1) != std::wstring::npos) return false;

    std::wstring yuan = dot == std::wstring::npos ? input : input.substr(0, dot);
    std::wstring fraction = dot == std::wstring::npos ? L"" : input.substr(dot + 1);
    if (yuan.empty()) yuan = L"0";
    if (fraction.size() > 2 || (dot != std::wstring::npos && fraction.empty())) return false;
    for (wchar_t ch : yuan) if (!iswdigit(ch)) return false;
    for (wchar_t ch : fraction) if (!iswdigit(ch)) return false;

    try {
        const long long yuanValue = std::stoll(yuan);
        long long fractionValue = fraction.empty() ? 0 : std::stoll(fraction);
        if (fraction.size() == 1) fractionValue *= 10;
        if (yuanValue > (LLONG_MAX - fractionValue) / 100) return false;
        cents = yuanValue * 100 + fractionValue;
        return cents > 0;
    } catch (...) {
        return false;
    }
}

bool isSixDigitNumber(const std::wstring& value) {
    if (value.size() != 6) return false;
    return std::all_of(value.begin(), value.end(), [](wchar_t ch) { return iswdigit(ch) != 0; });
}

bool isSixDigitPin(const std::wstring& pin) {
    return isSixDigitNumber(pin);
}

bool isElevenDigitPhone(const std::wstring& phone) {
    if (phone.size() != 11) return false;
    return std::all_of(phone.begin(), phone.end(), [](wchar_t ch) { return iswdigit(ch) != 0; });
}

std::string nowString() {
    const std::time_t current = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &current);
#else
    localtime_r(&current, &local);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
}
//二级菜单
std::wstring statementTypeName(StatementType type) {
    switch (type) {
        case StatementType::Deposit: return L"存款";
        case StatementType::Withdraw: return L"取款";
        case StatementType::TransferOut: return L"转账支出";
        case StatementType::TransferIn: return L"转账收入";
        case StatementType::PasswordChanged: return L"修改密码";
    }
    return L"其他";
}

Account* currentAccount() {
    return g_store.findAccount(g_currentAccountId);
}

void showError(const std::wstring& message) {
    MessageBoxW(g_window, message.c_str(), L"操作失败", MB_OK | MB_ICONWARNING);
}

void showInfo(const std::wstring& message) {
    MessageBoxW(g_window, message.c_str(), L"操作成功", MB_OK | MB_ICONINFORMATION);
}

bool saveStore() {
    std::string error;
    if (!g_store.save(error)) {
        showError(utf8ToWide(error));
        return false;
    }
    return true;
}

void clearPage() {
    for (HWND control : g_pageControls) DestroyWindow(control);
    g_pageControls.clear();
}

// 每个业务页面复用同一个主窗口，只替换内容区控件。
void addPageHeader(const std::wstring& title, const std::wstring& subtitle = L"") {
    addLabel(title, 60, 35, 820, 44);
    if (!subtitle.empty()) addLabel(subtitle, 60, 82, 820, 32);
}

void showLogin();
void showOpenAccount();
void showResetPassword();
void showMenu();
void showBalance();
void showDeposit();
void showWithdraw();
void showTransfer();
void showStatements();
void showChangePin();
void showDeleteAccount();

// 创建固定的数据目录，并把旧版本工作目录中的文件迁移过来。
bool configureDataStore() {
    std::wstring modulePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) return false;
    modulePath.resize(length);

    std::error_code filesystemError;
    const std::filesystem::path executableDirectory = std::filesystem::path(modulePath).parent_path();
    const std::filesystem::path fixedDirectory = executableDirectory / L"data";
    std::filesystem::create_directories(fixedDirectory, filesystemError);
    if (filesystemError) return false;

    // Preserve data created by older versions in the previous working directory.
    const std::filesystem::path oldDirectory = std::filesystem::current_path(filesystemError);
    const std::filesystem::path previousFixedDirectory = executableDirectory / L"atm_data";
    if (!filesystemError) {
        const std::vector<std::filesystem::path> oldDirectories = {oldDirectory, previousFixedDirectory};
        for (const wchar_t* fileName : {L"accounts.dat", L"statements.dat"}) {
            const auto newFile = fixedDirectory / fileName;
            if (std::filesystem::exists(newFile)) continue;
            for (const auto& directory : oldDirectories) {
                const auto oldFile = directory / fileName;
                if (directory == fixedDirectory || !std::filesystem::exists(oldFile)) continue;
                std::error_code copyError;
                std::filesystem::copy_file(oldFile, newFile, std::filesystem::copy_options::none, copyError);
                if (!copyError) break;
            }
        }
    }

    // main 分支继续使用 accounts.dat/statements.dat 文件保存业务数据。
    g_store = DataStore(fixedDirectory.u8string());
    return true;
}
//初始登录菜单
void showLogin() {
    clearPage();
    g_page = Page::Login;
    addPageHeader(L"ATM 仿真系统", L"请输入卡号（账户 ID）和 6 位密码登录");
    addLabel(L"卡号 / 账户 ID", 230, 175, 160, 32);
    addEdit(L"", IDC_LOGIN_ID, 405, 170, 300, 36);
    addLabel(L"密码", 230, 235, 160, 32);
    addEdit(L"", IDC_LOGIN_PIN, 405, 230, 300, 36, true);
    addButton(L"登录", IDC_LOGIN_BUTTON, 405, 320, 140, 44);
    addButton(L"找回密码", IDC_FORGOT_PASSWORD_BUTTON, 565, 320, 140, 44);
    addButton(L"开户", IDC_OPEN_ACCOUNT_BUTTON, 405, 390, 140, 44);
    addButton(L"退出程序", IDM_EXIT, 565, 390, 140, 44);
    addLabel(L"演示账户：100001（初始密码可在首次初始化时查看源码或向管理员索取）；其余样本账户初始密码相同",
             180, 480, 700, 32);
}
//开户菜单
void showOpenAccount() {
    clearPage();
    g_page = Page::OpenAccount;
    addPageHeader(L"开立账户", L"请输入自选的 6 位数字账户 ID；新账户初始余额为 ¥0.00");
    addLabel(L"账户 ID", 210, 125, 170, 32);
    addEdit(L"", IDC_OPEN_ID, 390, 120, 300, 36);
    addLabel(L"姓名", 210, 185, 170, 32);
    addEdit(L"", IDC_OPEN_NAME, 390, 180, 300, 36);
    addLabel(L"手机号", 210, 245, 170, 32);
    addEdit(L"", IDC_OPEN_PHONE, 390, 240, 300, 36);
    addLabel(L"登录密码", 210, 305, 170, 32);
    addEdit(L"", IDC_OPEN_PIN, 390, 300, 300, 36, true);
    addLabel(L"确认密码", 210, 365, 170, 32);
    addEdit(L"", IDC_OPEN_CONFIRM_PIN, 390, 360, 300, 36, true);
    addButton(L"确认开户", IDC_OPEN_BUTTON, 390, 440, 140, 44);
    addButton(L"返回登录", IDC_BACK_BUTTON, 550, 440, 140, 44);
}
//修改密码菜单
void showResetPassword() {
    clearPage();
    g_page = Page::ResetPassword;
    addPageHeader(L"找回密码", L"输入账户 ID 和绑定手机号尾号 4 位后设置新密码");
    addLabel(L"账户 ID", 210, 155, 170, 32);
    addEdit(L"", IDC_RESET_ID, 390, 150, 300, 36);
    addLabel(L"手机尾号 4 位", 210, 215, 170, 32);
    addEdit(L"", IDC_RESET_PHONE, 390, 210, 300, 36);
    addLabel(L"新密码", 210, 275, 170, 32);
    addEdit(L"", IDC_RESET_NEW_PIN, 390, 270, 300, 36, true);
    addLabel(L"确认新密码", 210, 335, 170, 32);
    addEdit(L"", IDC_RESET_CONFIRM_PIN, 390, 330, 300, 36, true);
    addButton(L"确认重设", IDC_RESET_BUTTON, 390, 410, 140, 44);
    addButton(L"返回登录", IDC_BACK_BUTTON, 550, 410, 140, 44);
}

void showMenu() {
    clearPage();
    g_page = Page::Menu;
    const Account* account = currentAccount();
    addPageHeader(L"ATM 主菜单", L"请选择要办理的业务");
    if (account) {
        addLabel(L"当前账户：" + utf8ToWide(account->name) + L"（" + utf8ToWide(account->id) + L"）",
                 60, 125, 700, 32);
    }
    addButton(L"账户查询", IDM_BALANCE, 160, 190, 190, 48);
    addButton(L"存款", IDM_DEPOSIT, 390, 190, 190, 48);
    addButton(L"取款", IDM_WITHDRAW, 620, 190, 190, 48);
    addButton(L"转账", IDM_TRANSFER, 160, 270, 190, 48);
    addButton(L"交易流水", IDM_STATEMENTS, 390, 270, 190, 48);
    addButton(L"修改密码", IDM_CHANGE_PIN, 620, 270, 190, 48);
    addButton(L"注销账户", IDM_DELETE_ACCOUNT, 160, 370, 190, 48);
    addButton(L"退出登录", IDM_LOGOUT, 390, 370, 190, 48);
}
//账户查询菜单
void showBalance() {
    clearPage();
    g_page = Page::Balance;
    const Account* account = currentAccount();
    addPageHeader(L"账户查询", L"当前账户信息");
    if (account) {
        addLabel(L"账户 ID：" + utf8ToWide(account->id), 230, 180, 500, 34);
        addLabel(L"账户姓名：" + utf8ToWide(account->name), 230, 230, 500, 34);
        addLabel(L"账户余额：" + formatMoney(account->balanceCents), 230, 280, 500, 40);
    }
    addButton(L"返回主菜单", IDC_BACK_BUTTON, 390, 390, 180, 44);
}
//存款菜单
void showDeposit() {
    clearPage();
    g_page = Page::Deposit;
    addPageHeader(L"存款", L"请输入存款金额，支持 0.01 元精度");
    addLabel(L"存款金额", 230, 190, 150, 32);
    addEdit(L"", IDC_DEPOSIT_AMOUNT, 390, 185, 300, 36);
    addButton(L"确认存款", IDC_DEPOSIT_BUTTON, 390, 270, 140, 44);
    addButton(L"返回", IDC_BACK_BUTTON, 550, 270, 140, 44);
}
//取款菜单
void showWithdraw() {
    clearPage();
    g_page = Page::Withdraw;
    addPageHeader(L"取款", L"请输入取款金额，余额不足时操作会被拒绝");
    addLabel(L"取款金额", 230, 190, 150, 32);
    addEdit(L"", IDC_WITHDRAW_AMOUNT, 390, 185, 300, 36);
    addButton(L"确认取款", IDC_WITHDRAW_BUTTON, 390, 270, 140, 44);
    addButton(L"返回", IDC_BACK_BUTTON, 550, 270, 140, 44);
}
//转账菜单
void showTransfer() {
    clearPage();
    g_page = Page::Transfer;
    addPageHeader(L"转账", L"请输入目标账户和转账金额");
    addLabel(L"目标账户 ID", 210, 175, 170, 32);
    addEdit(L"", IDC_TRANSFER_TARGET, 390, 170, 300, 36);
    addLabel(L"转账金额", 210, 235, 170, 32);
    addEdit(L"", IDC_TRANSFER_AMOUNT, 390, 230, 300, 36);
    addButton(L"确认转账", IDC_TRANSFER_BUTTON, 390, 315, 140, 44);
    addButton(L"返回", IDC_BACK_BUTTON, 550, 315, 140, 44);
}
//流水菜单
void showStatements() {
    clearPage();
    g_page = Page::Statements;
    addPageHeader(L"交易流水", L"最近的交易记录");
    HWND list = addControl(L"LISTBOX", L"", WS_BORDER | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
                           100, 135, 780, 350, 0, WS_EX_CLIENTEDGE);
    if (list) {
        for (const auto& statement : g_store.statements) {
            if (statement.accountId != g_currentAccountId) continue;
            std::wstring row = utf8ToWide(statement.time) + L"  " + statementTypeName(statement.type);
            if (statement.type != StatementType::PasswordChanged) {
                row += L"  " + formatMoney(statement.amountCents);
            }
            if (!statement.toAccount.empty()) row += L"  对方：" + utf8ToWide(statement.toAccount);
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
        }
    }
    addButton(L"返回主菜单", IDC_BACK_BUTTON, 390, 530, 180, 44);
}
//修改密码菜单
void showChangePin() {
    clearPage();
    g_page = Page::ChangePin;
    addPageHeader(L"修改密码", L"密码必须为 6 位数字");
    addLabel(L"旧密码", 230, 165, 150, 32);
    addEdit(L"", IDC_OLD_PIN, 390, 160, 300, 36, true);
    addLabel(L"新密码", 230, 225, 150, 32);
    addEdit(L"", IDC_NEW_PIN, 390, 220, 300, 36, true);
    addLabel(L"确认新密码", 230, 285, 150, 32);
    addEdit(L"", IDC_CONFIRM_PIN, 390, 280, 300, 36, true);
    addButton(L"确认修改", IDC_CHANGE_PIN_BUTTON, 390, 365, 140, 44);
    addButton(L"返回", IDC_BACK_BUTTON, 550, 365, 140, 44);
}
//销户菜单
void showDeleteAccount() {
    clearPage();
    g_page = Page::DeleteAccount;
    addPageHeader(L"注销账户", L"注销后账户、余额和关联流水都会被永久删除");
    addLabel(L"当前密码", 230, 205, 150, 32);
    addEdit(L"", IDC_DELETE_PASSWORD, 390, 200, 300, 36, true);
    addLabel(L"该操作不可恢复，请输入当前密码确认。", 230, 275, 500, 32);
    addButton(L"确认注销", IDC_DELETE_BUTTON, 390, 345, 140, 44);
    addButton(L"返回", IDC_BACK_BUTTON, 550, 345, 140, 44);
}

void handleLogin() {
    const std::string id = wideToUtf8(readText(GetDlgItem(g_window, IDC_LOGIN_ID)));
    const std::string pin = wideToUtf8(readText(GetDlgItem(g_window, IDC_LOGIN_PIN)));
    Account* account = g_store.findAccount(id);
    if (!account) {
        showError(L"账户不存在。\n请检查卡号或账户 ID。\n当前为演示程序，不会吞没未知卡号。");
        return;
    }
    if (account->status == CardStatus::Retained) {
        showError(L"该卡已被吞没，请联系银行工作人员。\n本程序不支持退卡操作。");
        return;
    }
    // 持久化文件中只保存 bcrypt 哈希：用“用户输入 + 文件中保存的盐”重新散列后比对。
    if (!bcrypt::verifyPassword(pin, account->pinHash)) {
        ++g_loginFailures;
        if (g_loginFailures >= 3) {
            account->status = CardStatus::Retained;
            saveStore();
            g_loginFailures = 0;
            showError(L"密码错误已达到 3 次，卡已被吞没。");
            showLogin();
        } else {
            showError(L"密码错误，还可以尝试 " + std::to_wstring(3 - g_loginFailures) + L" 次。");
        }
        return;
    }
    g_currentAccountId = id;
    g_loginFailures = 0;
    showMenu();
}

// 开户时由用户选择唯一的六位账户 ID，余额固定从零开始。
void handleOpenAccount() {
    const std::wstring idInput = readText(GetDlgItem(g_window, IDC_OPEN_ID));
    const std::wstring name = readText(GetDlgItem(g_window, IDC_OPEN_NAME));
    const std::wstring phoneInput = readText(GetDlgItem(g_window, IDC_OPEN_PHONE));
    const std::wstring pin = readText(GetDlgItem(g_window, IDC_OPEN_PIN));
    const std::wstring confirmPin = readText(GetDlgItem(g_window, IDC_OPEN_CONFIRM_PIN));
    if (!isSixDigitNumber(idInput)) {
        showError(L"账户 ID 必须是 6 位数字。");
        return;
    }
    const std::string id = wideToUtf8(idInput);
    if (g_store.findAccount(id)) {
        showError(L"该账户 ID 已存在，请选择其他 6 位数字。");
        return;
    }
    if (name.empty()) {
        showError(L"请输入账户姓名。");
        return;
    }
    if (name.find(L'|') != std::wstring::npos) {
        showError(L"姓名不能包含字符 |。");
        return;
    }
    if (!isElevenDigitPhone(phoneInput)) {
        showError(L"请输入 11 位数字手机号。");
        return;
    }
    const std::string phone = wideToUtf8(phoneInput);
    for (const auto& [id, account] : g_store.accounts) {
        if (account.phone == phone) {
            showError(L"该手机号已经绑定其他账户。");
            return;
        }
    }
    if (!isSixDigitPin(pin)) {
        showError(L"登录密码必须为 6 位数字。");
        return;
    }
    if (pin != confirmPin) {
        showError(L"两次输入的密码不一致。");
        return;
    }

    // 明文密码只在内存中短暂停留，写入缓存前立即做 bcrypt 散列。
    const std::string pinHash = bcrypt::hashPassword(wideToUtf8(pin));
    if (pinHash.empty()) {
        showError(L"密码加密失败，当前系统随机数不可用，请稍后重试。");
        return;
    }
    const Account newAccount{id, wideToUtf8(name), pinHash, phone, 0, CardStatus::Normal};
    g_store.accounts.emplace(id, newAccount);
    if (!saveStore()) {
        g_store.accounts.erase(id);
        return;
    }

    showInfo(L"开户成功！\n您的账户 ID 为：" + utf8ToWide(id) +
             L"\n初始余额：¥0.00");
    showLogin();
}

// 通过账户 ID、手机号尾号和新密码完成密码重设。
void handleResetPassword() {
    const std::string id = wideToUtf8(readText(GetDlgItem(g_window, IDC_RESET_ID)));
    const std::wstring phoneTail = readText(GetDlgItem(g_window, IDC_RESET_PHONE));
    const std::wstring newPin = readText(GetDlgItem(g_window, IDC_RESET_NEW_PIN));
    const std::wstring confirmPin = readText(GetDlgItem(g_window, IDC_RESET_CONFIRM_PIN));
    Account* account = g_store.findAccount(id);
    if (!account) {
        showError(L"账户不存在。");
        return;
    }
    if (account->status == CardStatus::Retained) {
        showError(L"该卡已被吞没，不能通过找回密码恢复，请联系银行工作人员。");
        return;
    }
    if (phoneTail.size() != 4 ||
        !std::all_of(phoneTail.begin(), phoneTail.end(), [](wchar_t ch) { return iswdigit(ch) != 0; })) {
        showError(L"请输入手机号尾号的 4 位数字。");
        return;
    }
    const std::string phone = account->phone;
    if (phone.size() < 4 || phone.substr(phone.size() - 4) != wideToUtf8(phoneTail)) {
        showError(L"手机号尾号验证失败。");
        return;
    }
    if (!isSixDigitPin(newPin)) {
        showError(L"新密码必须是 6 位数字。");
        return;
    }
    if (newPin != confirmPin) {
        showError(L"两次输入的新密码不一致。");
        return;
    }

    const std::string newPinHash = bcrypt::hashPassword(wideToUtf8(newPin));
    if (newPinHash.empty()) {
        showError(L"密码加密失败，当前系统随机数不可用，请稍后重试。");
        return;
    }
    const std::string oldPinHash = account->pinHash;
    account->pinHash = newPinHash;
    g_store.addStatement({g_store.nextStatementId(), account->id, nowString(), StatementType::PasswordChanged, 0, ""});
    if (!saveStore()) {
        account->pinHash = oldPinHash;
        g_store.removeLastStatement();
        return;
    }
    showInfo(L"密码重设成功，请使用新密码登录。");
    showLogin();
}

void handleDeposit() {
    long long amount = 0;
    if (!parseAmount(readText(GetDlgItem(g_window, IDC_DEPOSIT_AMOUNT)), amount)) {
        showError(L"请输入大于 0 的合法金额，例如 100 或 100.50。");
        return;
    }
    Account* account = currentAccount();
    if (!account) return;
    if (amount > LLONG_MAX - account->balanceCents) {
        showError(L"金额过大，无法完成存款。");
        return;
    }
    account->balanceCents += amount;
    g_store.addStatement({g_store.nextStatementId(), account->id, nowString(), StatementType::Deposit, amount, ""});
    if (!saveStore()) {
        account->balanceCents -= amount;
        g_store.removeLastStatement();
        return;
    }
    showInfo(L"存款成功，当前余额：" + formatMoney(account->balanceCents));
    showMenu();
}

void handleWithdraw() {
    long long amount = 0;
    if (!parseAmount(readText(GetDlgItem(g_window, IDC_WITHDRAW_AMOUNT)), amount)) {
        showError(L"请输入大于 0 的合法金额，例如 100 或 100.50。");
        return;
    }
    Account* account = currentAccount();
    if (!account) return;
    if (amount > account->balanceCents) {
        showError(L"余额不足，当前余额：" + formatMoney(account->balanceCents));
        return;
    }
    account->balanceCents -= amount;
    g_store.addStatement({g_store.nextStatementId(), account->id, nowString(), StatementType::Withdraw, amount, ""});
    if (!saveStore()) {
        account->balanceCents += amount;
        g_store.removeLastStatement();
        return;
    }
    showInfo(L"取款成功，当前余额：" + formatMoney(account->balanceCents));
    showMenu();
}

void handleTransfer() {
    long long amount = 0;
    const std::string targetId = wideToUtf8(readText(GetDlgItem(g_window, IDC_TRANSFER_TARGET)));
    if (!parseAmount(readText(GetDlgItem(g_window, IDC_TRANSFER_AMOUNT)), amount)) {
        showError(L"请输入大于 0 的合法金额。");
        return;
    }
    Account* source = currentAccount();
    Account* target = g_store.findAccount(targetId);
    if (!source || !target) {
        showError(L"目标账户不存在。");
        return;
    }
    if (source->id == target->id) {
        showError(L"不能向自己转账。");
        return;
    }
    if (amount > source->balanceCents) {
        showError(L"余额不足，当前余额：" + formatMoney(source->balanceCents));
        return;
    }
    if (amount > LLONG_MAX - target->balanceCents) {
        showError(L"目标账户余额达到上限，无法完成转账。");
        return;
    }

    source->balanceCents -= amount;
    target->balanceCents += amount;
    const std::string time = nowString();
    g_store.addStatement({g_store.nextStatementId(), source->id, time, StatementType::TransferOut, amount, target->id});
    g_store.addStatement({g_store.nextStatementId(), target->id, time, StatementType::TransferIn, amount, source->id});
    if (!saveStore()) {
        source->balanceCents += amount;
        target->balanceCents -= amount;
        g_store.removeLastStatement();
        g_store.removeLastStatement();
        return;
    }
    showInfo(L"转账成功，当前余额：" + formatMoney(source->balanceCents));
    showMenu();
}

void handleChangePin() {
    Account* account = currentAccount();
    if (!account) return;
    const std::wstring oldPin = readText(GetDlgItem(g_window, IDC_OLD_PIN));
    const std::wstring newPin = readText(GetDlgItem(g_window, IDC_NEW_PIN));
    const std::wstring confirmPin = readText(GetDlgItem(g_window, IDC_CONFIRM_PIN));
    if (!bcrypt::verifyPassword(wideToUtf8(oldPin), account->pinHash)) {
        showError(L"旧密码不正确。");
        return;
    }
    if (!isSixDigitPin(newPin)) {
        showError(L"新密码必须是 6 位数字。");
        return;
    }
    if (newPin != confirmPin) {
        showError(L"两次输入的新密码不一致。");
        return;
    }
    if (newPin == oldPin) {
        showError(L"新密码不能与旧密码相同。");
        return;
    }

    const std::string newPinHash = bcrypt::hashPassword(wideToUtf8(newPin));
    if (newPinHash.empty()) {
        showError(L"密码加密失败，当前系统随机数不可用，请稍后重试。");
        return;
    }
    const std::string oldPinHash = account->pinHash;
    account->pinHash = newPinHash;
    g_store.addStatement({g_store.nextStatementId(), account->id, nowString(), StatementType::PasswordChanged, 0, ""});
    if (!saveStore()) {
        account->pinHash = oldPinHash;
        g_store.removeLastStatement();
        return;
    }
    showInfo(L"密码修改成功，请妥善保管新密码。");
    showMenu();
}

void handleDeleteAccount() {
    Account* account = currentAccount();
    if (!account) return;
    const std::string password = wideToUtf8(readText(GetDlgItem(g_window, IDC_DELETE_PASSWORD)));
    if (!bcrypt::verifyPassword(password, account->pinHash)) {
        showError(L"当前密码不正确，不能注销账户。");
        return;
    }

    const std::wstring warning = L"确认注销账户 " + utf8ToWide(account->id) + L"？\n"
                                 L"账户余额 " + formatMoney(account->balanceCents) +
                                 L" 以及全部关联流水都会被永久删除。";
    if (MessageBoxW(g_window, warning.c_str(), L"确认注销账户", MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }

    const Account removedAccount = *account;
    const std::vector<Statement> oldStatements = g_store.statements;
    g_store.accounts.erase(removedAccount.id);
    g_store.statements.erase(
        std::remove_if(g_store.statements.begin(), g_store.statements.end(),
                       [&](const Statement& statement) { return statement.accountId == removedAccount.id; }),
        g_store.statements.end());
    if (!saveStore()) {
        g_store.accounts.emplace(removedAccount.id, removedAccount);
        g_store.statements = oldStatements;
        return;
    }

    g_currentAccountId.clear();
    g_loginFailures = 0;
    showInfo(L"账户已注销，相关数据已删除。");
    showLogin();
}

HMENU createMenuBar() {
    HMENU menuBar = CreateMenu();
    HMENU accountMenu = CreatePopupMenu();
    AppendMenuW(accountMenu, MF_STRING, IDM_BALANCE, L"账户查询");
    AppendMenuW(accountMenu, MF_STRING, IDM_DEPOSIT, L"存款");
    AppendMenuW(accountMenu, MF_STRING, IDM_WITHDRAW, L"取款");
    AppendMenuW(accountMenu, MF_STRING, IDM_TRANSFER, L"转账");
    AppendMenuW(accountMenu, MF_STRING, IDM_STATEMENTS, L"交易流水");
    AppendMenuW(accountMenu, MF_STRING, IDM_CHANGE_PIN, L"修改密码");
    AppendMenuW(accountMenu, MF_STRING, IDM_DELETE_ACCOUNT, L"注销账户");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(accountMenu), L"账户操作");

    HMENU sessionMenu = CreatePopupMenu();
    AppendMenuW(sessionMenu, MF_STRING, IDM_LOGOUT, L"退出登录");
    AppendMenuW(sessionMenu, MF_STRING, IDM_EXIT, L"退出程序");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(sessionMenu), L"系统");
    return menuBar;
}

void navigate(int command) {
    if (command == IDM_EXIT) {
        DestroyWindow(g_window);
        return;
    }
    if (command == IDM_LOGOUT) {
        g_currentAccountId.clear();
        g_loginFailures = 0;
        showLogin();
        return;
    }
    if (g_currentAccountId.empty()) {
        showError(L"请先登录账户。");
        return;
    }
    switch (command) {
        case IDM_BALANCE: showBalance(); break;
        case IDM_DEPOSIT: showDeposit(); break;
        case IDM_WITHDRAW: showWithdraw(); break;
        case IDM_TRANSFER: showTransfer(); break;
        case IDM_STATEMENTS: showStatements(); break;
        case IDM_CHANGE_PIN: showChangePin(); break;
        case IDM_DELETE_ACCOUNT: showDeleteAccount(); break;
        default: break;
    }
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            g_window = window;
            g_font = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
            SetMenu(window, createMenuBar());
            showLogin();
            return 0;
        case WM_COMMAND: {
            const int command = LOWORD(wParam);
            if (command == IDC_LOGIN_BUTTON) handleLogin();
            else if (command == IDC_FORGOT_PASSWORD_BUTTON) showResetPassword();
            else if (command == IDC_OPEN_ACCOUNT_BUTTON) showOpenAccount();
            else if (command == IDC_OPEN_BUTTON) handleOpenAccount();
            else if (command == IDC_RESET_BUTTON) handleResetPassword();
            else if (command == IDC_DEPOSIT_BUTTON) handleDeposit();
            else if (command == IDC_WITHDRAW_BUTTON) handleWithdraw();
            else if (command == IDC_TRANSFER_BUTTON) handleTransfer();
            else if (command == IDC_CHANGE_PIN_BUTTON) handleChangePin();
            else if (command == IDC_DELETE_BUTTON) handleDeleteAccount();
            else if (command == IDC_BACK_BUTTON &&
                     (g_page == Page::ResetPassword || g_page == Page::OpenAccount)) showLogin();
            else if (command == IDC_BACK_BUTTON) showMenu();
            else if (command >= IDM_BALANCE && command <= IDM_EXIT) navigate(command);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
        case WM_DESTROY:
            clearPage();
            if (g_font) DeleteObject(g_font);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}
}

int APIENTRY WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    INITCOMMONCONTROLSEX commonControls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&commonControls);

    if (!configureDataStore()) {
        MessageBoxW(nullptr, L"无法确定 ATM 数据保存目录。", L"ATM 数据初始化失败", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::string error;
    if (!g_store.load(error)) {
        MessageBoxW(nullptr, utf8ToWide(error).c_str(), L"ATM 数据初始化失败", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.lpszClassName = WINDOW_CLASS_NAME;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    if (!RegisterClassExW(&windowClass)) return 1;

    g_window = CreateWindowExW(0, WINDOW_CLASS_NAME, WINDOW_TITLE,
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               1000, 700, nullptr, nullptr, instance, nullptr);
    if (!g_window) return 1;

    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
