#include "computermodel.h"

#include <algorithm>

#include <QHash>
#include <QHostAddress>
#include <QHostInfo>
#include <QMutex>
#include <QNetworkInterface>
#include <QSharedPointer>
#include <QThreadPool>
#include <QWaitCondition>

#include <Limelight.h>  // VipleVr.h：VIPLE_VR_SERVER_CAP_MULTILINK

ComputerModel::ComputerModel(QObject* object)
    : QAbstractListModel(object) {}

void ComputerModel::initialize(ComputerManager* computerManager)
{
    m_ComputerManager = computerManager;
    connect(m_ComputerManager, &ComputerManager::computerStateChanged,
            this, &ComputerModel::handleComputerStateChanged);
    connect(m_ComputerManager, &ComputerManager::pairingCompleted,
            this, &ComputerModel::handlePairingCompleted);

    m_Computers = visibleComputers();
}

// §UI-HIDE-SELF（2026-10-29）：這台電腦自己也跑著 server 時，區網探索會把它加進主機列表。
// 只影響主機頁：儲存的主機不刪，命令列直接指定位址串流照常可用。
// §REVIEW-1104：名稱相同、舊的區網位址或單一 loopback 不能單獨藏起整台；要名稱相同且目前連線的 activeAddress 是本機
bool ComputerModel::isThisMachine(const NvComputer* computer)
{
    static const QString s_LocalName = QHostInfo::localHostName();
    QString name;
    NvAddress active;
    {
        QReadLocker lock(&computer->lock);
        name = computer->name;
        active = computer->activeAddress;
    }
    if (name.isEmpty() || s_LocalName.isEmpty() ||
            name.compare(s_LocalName, Qt::CaseInsensitive) != 0) {
        return false;
    }
    if (active.isNull()) {
        return false;
    }
    const QHostAddress host(active.address());
    if (host.isNull()) {
        return false;
    }
    if (host.isLoopback()) {
        return true;
    }
    const QList<QHostAddress> mine = QNetworkInterface::allAddresses();
    for (const QHostAddress& m : mine) {
        if (m.isEqual(host, QHostAddress::TolerantConversion)) {
            return true;
        }
    }
    return false;
}

int ComputerModel::managerIndex(int computerIndex) const
{
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) {
        return -1;
    }
    return m_ComputerManager->getComputers().indexOf(m_Computers[computerIndex]);
}

// §REVIEW-1104：依 UUID 反查目前列表中的列號，避免對話框開啟期間清單重排導致刪錯或改錯主機
int ComputerModel::indexForUuid(QString uuid) const
{
    if (uuid.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_Computers.count(); ++i) {
        NvComputer* c = m_Computers[i];
        if (c == nullptr) {
            continue;
        }
        QReadLocker lock(&c->lock);
        if (c->uuid == uuid) {
            return i;
        }
    }
    return -1;
}

QVector<NvComputer*> ComputerModel::visibleComputers() const
{
    QVector<NvComputer*> list = m_ComputerManager->getComputers();
    list.erase(std::remove_if(list.begin(), list.end(), [](NvComputer* c) { return c == nullptr || isThisMachine(c); }),
               list.end());
    return list;
}

QVariant ComputerModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }

    Q_ASSERT(index.row() < m_Computers.count());

    NvComputer* computer = m_Computers[index.row()];
    QReadLocker lock(&computer->lock);

    switch (role) {
    case NameRole:
        return computer->name;
    case OnlineRole:
        return computer->state == NvComputer::CS_ONLINE;
    case PairedRole:
        return computer->pairState == NvComputer::PS_PAIRED;
    case BusyRole:
        return computer->currentGameId != 0;
    case WakeableRole:
        return !computer->macAddress.isEmpty();
    case StatusUnknownRole:
        return computer->state == NvComputer::CS_UNKNOWN;
    case ServerSupportedRole:
        return computer->isSupportedServerVersion;
    case IsVipleStreamPeerRole:
        return computer->isVipleStreamPeer;
    case DirectLaunchDesktopRole:
        return computer->directLaunchDesktop;
    case ServerVersionRole:
        return computer->vipleStreamVersion;
    case MultiLinkRole:
        return (computer->vipleStreamVr & VIPLE_VR_SERVER_CAP_MULTILINK) != 0;
    // §REVIEW-1104：回傳主機 UUID 供 QML 對話框記錄目標主機
    case UuidRole:
        return computer->uuid;
    case DetailsRole: {
        QString state, pairState;

        switch (computer->state) {
        case NvComputer::CS_ONLINE:
            state = tr("Online");
            break;
        case NvComputer::CS_OFFLINE:
            state = tr("Offline");
            break;
        default:
            state = tr("Unknown");
            break;
        }

        switch (computer->pairState) {
        case NvComputer::PS_PAIRED:
            pairState = tr("Paired");
            break;
        case NvComputer::PS_NOT_PAIRED:
            pairState = tr("Unpaired");
            break;
        default:
            pairState = tr("Unknown");
            break;
        }

        return tr("Name: %1").arg(computer->name) + '\n' +
               tr("Status: %1").arg(state) + '\n' +
               tr("Active Address: %1").arg(computer->activeAddress.toString()) + '\n' +
               tr("UUID: %1").arg(computer->uuid) + '\n' +
               tr("Local Address: %1").arg(computer->localAddress.toString()) + '\n' +
               tr("Remote Address: %1").arg(computer->remoteAddress.toString()) + '\n' +
               tr("IPv6 Address: %1").arg(computer->ipv6Address.toString()) + '\n' +
               tr("Manual Address: %1").arg(computer->manualAddress.toString()) + '\n' +
               tr("MAC Address: %1").arg(computer->macAddress.isEmpty() ? tr("Unknown") : QString(computer->macAddress.toHex(':'))) + '\n' +
               tr("Pair State: %1").arg(pairState) + '\n' +
               tr("Running Game ID: %1").arg(computer->state == NvComputer::CS_ONLINE ? QString::number(computer->currentGameId) : tr("Unknown")) + '\n' +
               tr("HTTPS Port: %1").arg(computer->state == NvComputer::CS_ONLINE ? QString::number(computer->activeHttpsPort) : tr("Unknown"));
    }
    default:
        return QVariant();
    }
}

int ComputerModel::rowCount(const QModelIndex& parent) const
{
    // We should not return a count for valid index values,
    // only the parent (which will not have a "valid" index).
    if (parent.isValid()) {
        return 0;
    }

    return m_Computers.count();
}

int ComputerModel::featuredComputerIndex() const
{
    // Prefer an online + paired host.
    for (int i = 0; i < m_Computers.count(); ++i) {
        NvComputer* c = m_Computers[i];
        if (c && c->state == NvComputer::CS_ONLINE && c->pairState == NvComputer::PS_PAIRED) {
            return i;
        }
    }
    // Otherwise the first paired host (reachable-ish but may be offline).
    for (int i = 0; i < m_Computers.count(); ++i) {
        NvComputer* c = m_Computers[i];
        if (c && c->pairState == NvComputer::PS_PAIRED) {
            return i;
        }
    }
    // Fall back to row 0, or -1 for an empty list.
    return m_Computers.isEmpty() ? -1 : 0;
}

QString ComputerModel::nameAt(int index) const
{
    if (index < 0 || index >= m_Computers.count() || !m_Computers[index]) {
        return QString();
    }
    return m_Computers[index]->name;
}

QHash<int, QByteArray> ComputerModel::roleNames() const
{
    QHash<int, QByteArray> names;

    names[NameRole] = "name";
    names[OnlineRole] = "online";
    names[PairedRole] = "paired";
    names[BusyRole] = "busy";
    names[WakeableRole] = "wakeable";
    names[StatusUnknownRole] = "statusUnknown";
    names[ServerSupportedRole] = "serverSupported";
    names[IsVipleStreamPeerRole] = "isVipleStreamPeer";
    names[DirectLaunchDesktopRole] = "directLaunchDesktop";
    names[DetailsRole] = "details";
    names[ServerVersionRole] = "serverVersion";
    names[MultiLinkRole] = "multiLink";  // §UI-MULTILINK-BADGE
    // §REVIEW-1104：註冊 uuid role 名稱供 PcView.qml 讀取 model.uuid
    names[UuidRole] = "uuid";

    return names;
}

Session* ComputerModel::createSessionForCurrentGame(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    NvComputer* computer = m_Computers[computerIndex];

    // We must currently be streaming a game to use this function
    Q_ASSERT(computer->currentGameId != 0);

    for (NvApp& app : computer->appList) {
        if (app.id == computer->currentGameId) {
            return new Session(computer, app);
        }
    }

    // We have a current running app but it's not in our app list
    Q_ASSERT(false);
    return nullptr;
}

static void waitForComputerWakes(const NvComputer* computer);

void ComputerModel::deleteComputer(int computerIndex)
{
    // §REVIEW-1104：release 會拿掉 Q_ASSERT，列號越界不能拿去刪
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) {
        return;
    }

    NvComputer* computer = m_Computers[computerIndex];

    // §REVIEW-1104：這台的 wake 還握著裸指標，刪除前先等它跑完，避免和 delete 並行
    waitForComputerWakes(computer);

    beginRemoveRows(QModelIndex(), computerIndex, computerIndex);

    // m_Computer[computerIndex] will be deleted by this call
    m_ComputerManager->deleteHost(computer);

    // Remove the now invalid item
    m_Computers.removeAt(computerIndex);

    endRemoveRows();
}

struct WakeInflight {
    int count = 0;
    QMutex mutex;
    QWaitCondition done;
};

static QMutex s_WakeInflightLock;
static QHash<const NvComputer*, QSharedPointer<WakeInflight>> s_WakeInflight;

static QSharedPointer<WakeInflight> beginComputerWake(NvComputer* computer)
{
    QMutexLocker mapLock(&s_WakeInflightLock);
    QSharedPointer<WakeInflight>& slot = s_WakeInflight[computer];
    if (slot.isNull()) {
        slot = QSharedPointer<WakeInflight>::create();
    }
    QSharedPointer<WakeInflight> ticket = slot;
    QMutexLocker ticketLock(&ticket->mutex);
    ticket->count++;
    return ticket;
}

static void endComputerWake(const QSharedPointer<WakeInflight>& ticket)
{
    QMutexLocker ticketLock(&ticket->mutex);
    ticket->count--;
    if (ticket->count == 0) {
        ticket->done.wakeAll();
    }
}

static void waitForComputerWakes(const NvComputer* computer)
{
    QSharedPointer<WakeInflight> ticket;
    {
        QMutexLocker mapLock(&s_WakeInflightLock);
        ticket = s_WakeInflight.take(computer);
    }
    if (ticket.isNull()) {
        return;
    }
    QMutexLocker ticketLock(&ticket->mutex);
    while (ticket->count > 0) {
        ticket->done.wait(&ticket->mutex);
    }
}

class DeferredWakeHostTask : public QRunnable
{
public:
    DeferredWakeHostTask(NvComputer* computer, QSharedPointer<WakeInflight> ticket)
        : m_Computer(computer),
          m_Ticket(ticket) {}

    void run()
    {
        // §REVIEW-1104：wake 跟刪除會在同一個 thread pool 並行；跑完才放掉計數，刪除那邊才繼續
        m_Computer->wake();
        endComputerWake(m_Ticket);
    }

private:
    NvComputer* m_Computer;
    QSharedPointer<WakeInflight> m_Ticket;
};

void ComputerModel::wakeComputer(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    NvComputer* computer = m_Computers[computerIndex];

    // §REVIEW-1104：wake 任務帶計數，刪除這台前才等得到它結束
    DeferredWakeHostTask* wakeTask = new DeferredWakeHostTask(computer, beginComputerWake(computer));
    QThreadPool::globalInstance()->start(wakeTask);

    // VipleStream §K.X — also poke the polling thread for this host so it
    // switches back to fast cadence and surfaces the Online state quickly
    // (without this, autoWakeOnLan==false would leave the thread sleeping
    // ~60 s after the magic packet was sent — host wakes but UI looks dead).
    if (m_ComputerManager) {
        m_ComputerManager->notifyExplicitWake(computer->uuid);
    }
}

void ComputerModel::wakeAllOfflineHosts()
{
    // Batch-wake every paired+wakeable offline host.  Called from
    // PcView.qml when the user toggles autoWakeOnLan OFF→ON to make
    // the toggle's UX promise "ON = PCs page wakes my hosts" actually
    // hold without relying on side-effects of /serverinfo polling.
    for (NvComputer* computer : m_Computers) {
        QReadLocker lock(&computer->lock);
        if (computer->state != NvComputer::CS_ONLINE
                && !computer->macAddress.isEmpty()) {
            // §REVIEW-1104：同上，批次喚醒也要算進同一份計數
            DeferredWakeHostTask* wakeTask = new DeferredWakeHostTask(computer, beginComputerWake(computer));
            QThreadPool::globalInstance()->start(wakeTask);
            if (m_ComputerManager) {
                m_ComputerManager->notifyExplicitWake(computer->uuid);
            }
        }
    }
}

void ComputerModel::renameComputer(int computerIndex, QString name)
{
    // §REVIEW-1104：release 會拿掉 Q_ASSERT，列號越界不能拿去改名
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) {
        return;
    }

    m_ComputerManager->renameHost(m_Computers[computerIndex], name);
}

void ComputerModel::setDirectLaunchDesktop(int computerIndex, bool enabled)
{
    // VipleStream — per-host toggle: 點該 host tile 直接串流 Desktop（跳過 applist）。
    // 改完 NvComputer 欄位後呼叫公開的 clientSideAttributeUpdated()，它會
    // 經 handleComputerStateChanged 走「saveHost (mark dirty → DelayedFlushThread
    // 落盤) + emit computerStateChanged」鏈，後者觸發本模型 handleComputerStateChanged
    // → emit dataChanged，QML 自動 refresh，不必我手動 emit。
    Q_ASSERT(computerIndex < m_Computers.count());

    NvComputer* computer = m_Computers[computerIndex];
    {
        QWriteLocker lock(&computer->lock);
        if (computer->directLaunchDesktop == enabled) {
            return;  // no-op
        }
        computer->directLaunchDesktop = enabled;
    }

    m_ComputerManager->clientSideAttributeUpdated(computer);
}

QString ComputerModel::generatePinString()
{
    return m_ComputerManager->generatePinString();
}

class DeferredTestConnectionTask : public QObject, public QRunnable
{
    Q_OBJECT
public:
    void run()
    {
        unsigned int portTestResult = LiTestClientConnectivity("qt.conntest.moonlight-stream.org", 443, ML_PORT_FLAG_ALL);
        if (portTestResult == ML_TEST_RESULT_INCONCLUSIVE) {
            emit connectionTestCompleted(-1, QString());
        }
        else {
            char blockedPorts[512];
            LiStringifyPortFlags(portTestResult, "\n", blockedPorts, sizeof(blockedPorts));
            emit connectionTestCompleted(portTestResult, QString(blockedPorts));
        }
    }

signals:
    void connectionTestCompleted(int result, QString blockedPorts);
};

void ComputerModel::testConnectionForComputer(int)
{
    DeferredTestConnectionTask* testConnectionTask = new DeferredTestConnectionTask();
    QObject::connect(testConnectionTask, &DeferredTestConnectionTask::connectionTestCompleted,
                     this, &ComputerModel::connectionTestCompleted);
    QThreadPool::globalInstance()->start(testConnectionTask);
}

void ComputerModel::pairComputer(int computerIndex, QString pin)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    m_ComputerManager->pairHost(m_Computers[computerIndex], pin);
}

void ComputerModel::handlePairingCompleted(NvComputer*, QString error)
{
    emit pairingCompleted(error.isEmpty() ? QVariant() : error);
}

void ComputerModel::handleComputerStateChanged(NvComputer* computer)
{
    QVector<NvComputer*> newComputerList = visibleComputers();  // §UI-HIDE-SELF

    // Reset the model if the structural layout of the list has changed
    if (m_Computers != newComputerList) {
        beginResetModel();
        m_Computers = newComputerList;
        endResetModel();
    }
    else {
        // Let the view know that this specific computer changed
        int index = m_Computers.indexOf(computer);
        if (index >= 0) {  // §UI-HIDE-SELF：被濾掉的（這台電腦自己）不在列表裡
            emit dataChanged(createIndex(index, 0), createIndex(index, 0));
        }
    }
}

#include "computermodel.moc"
