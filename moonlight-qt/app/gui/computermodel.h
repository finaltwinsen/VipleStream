#include "backend/computermanager.h"
#include "streaming/session.h"

#include <QAbstractListModel>

class ComputerModel : public QAbstractListModel
{
    Q_OBJECT

    enum Roles
    {
        NameRole = Qt::UserRole,
        OnlineRole,
        PairedRole,
        BusyRole,
        WakeableRole,
        StatusUnknownRole,
        ServerSupportedRole,
        // VipleStream H.5: peer is running VipleStream-Server (carries the
        // <VipleStreamProtocol> tag in /serverinfo).  Used by PcView.qml
        // to render a small VipleStream badge on the host card.
        IsVipleStreamPeerRole,
        DetailsRole
    };

public:
    explicit ComputerModel(QObject* object = nullptr);

    // Must be called before any QAbstractListModel functions
    Q_INVOKABLE void initialize(ComputerManager* computerManager);

    QVariant data(const QModelIndex &index, int role) const override;

    int rowCount(const QModelIndex &parent) const override;

    virtual QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void deleteComputer(int computerIndex);

    Q_INVOKABLE QString generatePinString();

    Q_INVOKABLE void pairComputer(int computerIndex, QString pin);

    Q_INVOKABLE void testConnectionForComputer(int computerIndex);

    Q_INVOKABLE void wakeComputer(int computerIndex);

    Q_INVOKABLE void renameComputer(int computerIndex, QString name);

    Q_INVOKABLE Session* createSessionForCurrentGame(int computerIndex);

    // VipleStream: decoration helpers for the §01 Bold "featured host"
    // hero on PcView.qml.  Both return -1 / empty string for an empty
    // grid so QML can guard on that.
    //
    // featuredComputerIndex() picks, in order of preference:
    //   1. the first online + paired host
    //   2. otherwise the first paired host
    //   3. otherwise 0 (the first row)
    //   4. -1 if the list is empty
    Q_INVOKABLE int featuredComputerIndex() const;
    Q_INVOKABLE QString nameAt(int index) const;

signals:
    void pairingCompleted(QVariant error);
    void connectionTestCompleted(int result, QString blockedPorts);

private slots:
    void handleComputerStateChanged(NvComputer* computer);

    void handlePairingCompleted(NvComputer* computer, QString error);

private:
    QVector<NvComputer*> m_Computers;
    ComputerManager* m_ComputerManager;
};
