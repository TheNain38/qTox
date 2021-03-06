/*
    Copyright © 2014-2015 by The qTox Project

    This file is part of qTox, a Qt-based graphical interface for Tox.

    qTox is libre software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    qTox is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with qTox.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "historykeeper.h"
#include "src/persistence/settings.h"
#include "src/core/core.h"
#include "src/nexus.h"
#include "src/persistence/profile.h"

#include <QSqlError>
#include <QFile>
#include <QDir>
#include <QSqlQuery>
#include <QVariant>
#include <QBuffer>
#include <QDebug>
#include <QTemporaryFile>

#include "src/persistence/db/plaindb.h"
#include "src/persistence/db/encrypteddb.h"

static HistoryKeeper *historyInstance = nullptr;
QMutex HistoryKeeper::historyMutex;

HistoryKeeper *HistoryKeeper::getInstance()
{
    historyMutex.lock();
    if (historyInstance == nullptr)
    {
        QList<QString> initLst;
        initLst.push_back(QString("CREATE TABLE IF NOT EXISTS history (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, ") +
                          QString("chat_id INTEGER NOT NULL, sender INTEGER NOT NULL, message TEXT NOT NULL, alias TEXT);"));
        initLst.push_back(QString("CREATE TABLE IF NOT EXISTS aliases (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id TEXT UNIQUE NOT NULL);"));
        initLst.push_back(QString("CREATE TABLE IF NOT EXISTS chats (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE NOT NULL, ctype INTEGER NOT NULL);"));
        initLst.push_back(QString("CREATE TABLE IF NOT EXISTS sent_status (id INTEGER PRIMARY KEY AUTOINCREMENT, status INTEGER NOT NULL DEFAULT 0);"));

        QString path(":memory:");
        GenericDdInterface *dbIntf;

        if (Nexus::getProfile()->isEncrypted())
        {
            path = getHistoryPath();
            dbIntf = new EncryptedDb(path, initLst);

            historyInstance = new HistoryKeeper(dbIntf);
            historyMutex.unlock();
            return historyInstance;
        }
        else
        {
            path = getHistoryPath();
        }

        dbIntf = new PlainDb(path, initLst);
        historyInstance = new HistoryKeeper(dbIntf);
    }
    historyMutex.unlock();

    return historyInstance;
}

bool HistoryKeeper::checkPassword(const TOX_PASS_KEY &passkey, int encrypted)
{
    if (!Settings::getInstance().getEnableLogging() && (encrypted == -1))
        return true;

    if ((encrypted == 1) || (encrypted == -1))
        return EncryptedDb::check(passkey, getHistoryPath(Nexus::getProfile()->getName(), encrypted));

    return true;
}

HistoryKeeper::HistoryKeeper(GenericDdInterface *db_) :
    db(db_)
{
    /*
     DB format
     chats:
      * name -> id map
       id      -- auto-incrementing number
       name    -- chat's name (for user to user conversation it is opposite user public key)
       ctype   -- chat type, reserved for group chats

     aliases:
      * user_id -> id map
       id       -- auto-incrementing number
       user_id  -- user's public key
       av_hash  -- hash of user's avatar
       avatar   -- user's avatar

     history:
       id           -- auto-incrementing number
       timestamp
       chat_id      -- current chat ID (resolves from chats table)
       sender       -- sender's ID (resolves from aliases table)
       message
       alias        -- sender's alias in
    */

    // for old tables:
    QSqlQuery ans = db->exec("SELECT seq FROM sqlite_sequence WHERE name=\"history\";");
    if (ans.first())
    {
        int idMax = ans.value(0).toInt();
        QSqlQuery ret = db->exec("SELECT seq FROM sqlite_sequence WHERE name=\"sent_status\";");
        int idCur = 0;
        if (ret.first())
            idCur = ret.value(0).toInt();

        if (idCur != idMax)
        {
            QString cmd = QString("INSERT INTO sent_status (id, status) VALUES (%1, 1);").arg(idMax);
            db->exec(cmd);
        }
    }
    //check table stuct
    ans = db->exec("PRAGMA table_info (\"history\")");
    ans.seek(5);
    if (!ans.value(1).toString().contains("alias"))
    {
        //add collum in table
        db->exec("ALTER TABLE history ADD COLUMN alias TEXT");
        qDebug() << "Struct DB updated: Added column alias in table history.";
    }

    ans.clear();
    ans = db->exec("PRAGMA table_info('aliases')");
    ans.seek(2);
    if (!ans.value(1).toString().contains("av_hash"))
    {
        //add collum in table
        db->exec("ALTER TABLE aliases ADD COLUMN av_hash BLOB");
        qDebug() << "Struct DB updated: Added column av_hash in table aliases.";
    }

    ans.seek(3);
    if (!ans.value(1).toString().contains("avatar"))
    {
        //add collum in table
        needImport = true;
        db->exec("ALTER TABLE aliases ADD COLUMN avatar BLOB");
        qDebug() << "Struct DB updated: Added column avatar in table aliases.";
    }

    updateChatsID();
    updateAliases();

    setSyncType(Settings::getInstance().getDbSyncType());

    messageID = 0;
    QSqlQuery sqlAnswer = db->exec("SELECT seq FROM sqlite_sequence WHERE name=\"history\";");
    if (sqlAnswer.first())
        messageID = sqlAnswer.value(0).toLongLong();
}

void HistoryKeeper::importAvatarToDatabase(const QString& ownerId)
{
    if (needImport)
    {
        QString puth (Settings::getInstance().getSettingsDirPath() +
                      QString("avatars") + QDir::separator() +
                      ownerId +".png");
        qDebug() << QString("Try import avatar for: %1.").arg(ownerId);
        getAliasID(ownerId);
        if (QFile::exists(puth) && !hasAvatar(ownerId))
        {
            QPixmap pic(puth);
            saveAvatar(pic,ownerId);
            qDebug() << QString("Import avatar for: %1.").arg(ownerId);
        }
    }
}

HistoryKeeper::~HistoryKeeper()
{
    delete db;
}

void HistoryKeeper::removeFriendHistory(const QString& chat)
{
    int chat_id = getChatID(chat, ctSingle).first;

    db->exec("BEGIN TRANSACTION;");

    QString cmd = QString("DELETE FROM chats WHERE name = '%1';").arg(chat);
    db->exec(cmd);
    cmd = QString("DELETE FROM aliases WHERE user_id = '%1';").arg(chat);
    db->exec(cmd);
    cmd = QString("DELETE FROM sent_status WHERE id IN (SELECT id FROM history WHERE chat_id = '%1');").arg(chat_id);
    db->exec(cmd);
    cmd = QString("DELETE FROM history WHERE chat_id = '%1';").arg(chat_id);
    db->exec(cmd);

    db->exec("COMMIT TRANSACTION;");
}

qint64 HistoryKeeper::addChatEntry(const QString& chat, const QString& message, const QString& sender, const QDateTime &dt, bool isSent, QString dispName)
{
    QList<QString> cmds = generateAddChatEntryCmd(chat, message, sender, dt, isSent, dispName);

    db->exec("BEGIN TRANSACTION;");
    for (auto &it : cmds)
        db->exec(it);

    db->exec("COMMIT TRANSACTION;");

    messageID++;
    return messageID;
}

QList<HistoryKeeper::HistMessage> HistoryKeeper::getChatHistory(HistoryKeeper::ChatType ct, const QString &chat,
                                                                const QDateTime &time_from, const QDateTime &time_to)
{
    QList<HistMessage> res;

    qint64 time64_from = time_from.toMSecsSinceEpoch();
    qint64 time64_to = time_to.toMSecsSinceEpoch();

    int chat_id = getChatID(chat, ct).first;

    QSqlQuery dbAnswer;
    if (ct == ctSingle)
    {
        dbAnswer = db->exec(QString("SELECT history.id, timestamp, user_id, message, status, alias FROM history LEFT JOIN sent_status ON history.id = sent_status.id ") +
                            QString("INNER JOIN aliases ON history.sender = aliases.id AND timestamp BETWEEN %1 AND %2 AND chat_id = %3;")
                            .arg(time64_from).arg(time64_to).arg(chat_id));
    }
    else
    {
        // no groupchats yet
    }

    while (dbAnswer.next())
    {
        qint64 id = dbAnswer.value(0).toLongLong();
        qint64 timeInt = dbAnswer.value(1).toLongLong();
        QString sender = dbAnswer.value(2).toString();
        QString senderName = dbAnswer.value(5).toString();
        QString message = unWrapMessage(dbAnswer.value(3).toString());
        bool isSent = true;
        if (!dbAnswer.value(4).isNull())
            isSent = dbAnswer.value(4).toBool();

        QDateTime time = QDateTime::fromMSecsSinceEpoch(timeInt);

        res.push_back(HistMessage(id, "", sender, message, time, isSent, senderName));
    }

    return res;
}

QList<HistoryKeeper::HistMessage> HistoryKeeper::exportMessages()
{
    QSqlQuery dbAnswer;
    dbAnswer = db->exec(QString("SELECT history.id, timestamp, user_id, message, status, name, alias FROM history LEFT JOIN sent_status ON history.id = sent_status.id ") +
                        QString("INNER JOIN aliases ON history.sender = aliases.id INNER JOIN chats ON history.chat_id = chats.id;"));

    QList<HistMessage> res;

    while (dbAnswer.next())
    {
        qint64 id = dbAnswer.value(0).toLongLong();
        qint64 timeInt = dbAnswer.value(1).toLongLong();
        QString sender = dbAnswer.value(2).toString();
        QString message = unWrapMessage(dbAnswer.value(3).toString());
        bool isSent = true;
        if (!dbAnswer.value(4).isNull())
            isSent = dbAnswer.value(4).toBool();
        QString chat = dbAnswer.value(5).toString();
        QString dispName = dbAnswer.value(6).toString();
        QDateTime time = QDateTime::fromMSecsSinceEpoch(timeInt);

        res.push_back(HistMessage(id, chat, sender, message, time, isSent, dispName));
    }

    return res;
}

void HistoryKeeper::importMessages(const QList<HistoryKeeper::HistMessage> &lst)
{
    db->exec("BEGIN TRANSACTION;");
    for (const HistMessage &msg : lst)
    {
        QList<QString> cmds = generateAddChatEntryCmd(msg.chat, msg.message, msg.sender, msg.timestamp, msg.isSent, QString()); //!!!
        for (auto &it : cmds)
            db->exec(it);

        messageID++;
    }
    db->exec("COMMIT TRANSACTION;");
}

QList<QString> HistoryKeeper::generateAddChatEntryCmd(const QString& chat, const QString& message, const QString& sender, const QDateTime &dt, bool isSent, QString dispName)
{
    QList<QString> cmds;

    int chat_id = getChatID(chat, ctSingle).first;
    int sender_id = getAliasID(sender);

    cmds.push_back(QString("INSERT INTO history (timestamp, chat_id, sender, message, alias) VALUES (%1, %2, %3, '%4', '%5');")
                   .arg(dt.toMSecsSinceEpoch()).arg(chat_id).arg(sender_id).arg(wrapMessage(message.toUtf8())).arg(QString(dispName.toUtf8())));
    cmds.push_back(QString("INSERT INTO sent_status (status) VALUES (%1);").arg(isSent));

    return cmds;
}

QString HistoryKeeper::wrapMessage(const QString &str)
{
    QString wrappedMessage(str);
    wrappedMessage.replace("'", "''");
    return wrappedMessage;
}

QString HistoryKeeper::unWrapMessage(const QString &str)
{
    QString unWrappedMessage(str);
    unWrappedMessage.replace("''", "'");
    return unWrappedMessage;
}

void HistoryKeeper::updateChatsID()
{
    auto dbAnswer = db->exec(QString("SELECT * FROM chats;"));

    chats.clear();
    while (dbAnswer.next())
    {
        QString name = dbAnswer.value(1).toString();
        int id = dbAnswer.value(0).toInt();
        ChatType ctype = convertToChatType(dbAnswer.value(2).toInt());

        chats[name] = {id, ctype};
    }
}

void HistoryKeeper::updateAliases()
{
    auto dbAnswer = db->exec(QString("SELECT * FROM aliases;"));

    aliases.clear();
    while (dbAnswer.next())
    {
        QString user_id = dbAnswer.value(1).toString();
        int id = dbAnswer.value(0).toInt();

        aliases[user_id] = id;
    }
}

QPair<int, HistoryKeeper::ChatType> HistoryKeeper::getChatID(const QString &id_str, ChatType ct)
{
    auto it = chats.find(id_str);
    if (it != chats.end())
        return it.value();

    db->exec(QString("INSERT INTO chats (name, ctype) VALUES ('%1', '%2');").arg(id_str).arg(ct));
    updateChatsID();

    return getChatID(id_str, ct);
}

int HistoryKeeper::getAliasID(const QString &id_str)
{
    auto it = aliases.find(id_str);
    if (it != aliases.end())
        return it.value();

    db->exec(QString("INSERT INTO aliases (user_id) VALUES ('%1');").arg(id_str));
    updateAliases();

    return getAliasID(id_str);
}

void HistoryKeeper::resetInstance()
{
    if (historyInstance == nullptr)
        return;

    delete historyInstance;
    historyInstance = nullptr;
}

qint64 HistoryKeeper::addGroupChatEntry(const QString &chat, const QString &message, const QString &sender, const QDateTime &dt)
{
    Q_UNUSED(chat)
    Q_UNUSED(message)
    Q_UNUSED(sender)
    Q_UNUSED(dt)
    // no groupchats yet

    return -1;
}

HistoryKeeper::ChatType HistoryKeeper::convertToChatType(int ct)
{
    if (ct < 0 || ct > 1)
        return ctSingle;

    return static_cast<ChatType>(ct);
}

QString HistoryKeeper::getHistoryPath(QString currentProfile, int encrypted)
{
    QDir baseDir(Settings::getInstance().getSettingsDirPath());
    if (currentProfile.isEmpty())
        currentProfile = Settings::getInstance().getCurrentProfile();

    if (encrypted == 1 || (encrypted == -1 && Nexus::getProfile()->isEncrypted()))
        return baseDir.filePath(currentProfile + ".qtox_history.encrypted");
    else
        return baseDir.filePath(currentProfile + ".qtox_history");
}

void HistoryKeeper::renameHistory(QString from, QString to)
{
    resetInstance();

    QFile fileEnc(QDir(Settings::getInstance().getSettingsDirPath()).filePath(from + ".qtox_history.encrypted"));
    if (fileEnc.exists())
        fileEnc.rename(QDir(Settings::getInstance().getSettingsDirPath()).filePath(to + ".qtox_history.encrypted"));

    QFile filePlain(QDir(Settings::getInstance().getSettingsDirPath()).filePath(from + ".qtox_history"));
    if (filePlain.exists())
        filePlain.rename(QDir(Settings::getInstance().getSettingsDirPath()).filePath(to + ".qtox_history"));
}

void HistoryKeeper::markAsSent(int m_id)
{
    db->exec(QString("UPDATE sent_status SET status = 1 WHERE id = %1;").arg(m_id));
}

QDate HistoryKeeper::getLatestDate(const QString &chat)
{
    int chat_id = getChatID(chat, ctSingle).first;

    QSqlQuery dbAnswer;
    dbAnswer = db->exec(QString("SELECT MAX(timestamp) FROM history LEFT JOIN sent_status ON history.id = sent_status.id ") +
                        QString("INNER JOIN aliases ON history.sender = aliases.id AND chat_id = %3;")
                        .arg(chat_id));

    if (dbAnswer.first())
    {
        qint64 timeInt = dbAnswer.value(0).toLongLong();

        if (timeInt != 0)
            return QDateTime::fromMSecsSinceEpoch(timeInt).date();
    }

    return QDate();
}

void HistoryKeeper::setSyncType(Db::syncType sType)
{
    QString syncCmd;

    switch (sType)
    {
    case Db::syncType::stFull:
        syncCmd = "FULL";
        break;
    case Db::syncType::stNormal:
        syncCmd = "NORMAL";
        break;
    case Db::syncType::stOff:
        syncCmd = "OFF";
        break;
    default:
        syncCmd = "FULL";
        break;
    }

    db->exec(QString("PRAGMA synchronous=%1;").arg(syncCmd));
}

bool HistoryKeeper::isFileExist()
{
    QString path = getHistoryPath();
    QFile file(path);

    return file.exists();
}

void HistoryKeeper::removeHistory()
{
    db->exec("BEGIN TRANSACTION;");
    db->exec("DELETE FROM sent_status;");
    db->exec("DELETE FROM history;");
    db->exec("COMMIT TRANSACTION;");
}

QList<HistoryKeeper::HistMessage> HistoryKeeper::exportMessagesDeleteFile()
{
    auto msgs = getInstance()->exportMessages();
    qDebug() << "Messages exported";
    getInstance()->removeHistory();

    return msgs;
}

void HistoryKeeper::removeAvatar(const QString& ownerId)
{
    QSqlQuery query;
    query.prepare("UPDATE aliases SET avatar=NULL, av_hash=NULL WHERE user_id = (:id)");
    query.bindValue(":id", ownerId.left(64));
    query.exec();
}

bool HistoryKeeper::hasAvatar(const QString& ownerId)
{
    QSqlQuery sqlAnswer = db->exec(QString("SELECT avatar FROM aliases WHERE user_id= '%1'").arg(ownerId.left(64)));
    return !sqlAnswer.isNull(0);
}

void HistoryKeeper::saveAvatar(QPixmap& pic, const QString& ownerId)
{
    QByteArray bArray;
    QBuffer buffer(&bArray);
    buffer.open(QIODevice::WriteOnly);
    pic.save(&buffer, "PNG");

    QSqlQuery query;
    query.prepare("UPDATE aliases SET avatar=:image WHERE user_id = (:id)");
    query.bindValue(":image", bArray);
    query.bindValue(":id", ownerId.left(64));
    query.exec();
}

QPixmap HistoryKeeper::getSavedAvatar(const QString &ownerId)
{
    QByteArray bArray;
    QPixmap pixmap = QPixmap();
    QSqlQuery sqlAnswer = db->exec(QString("SELECT avatar FROM aliases WHERE user_id= '%1'").arg(ownerId.left(64)));
    if (sqlAnswer.first())
    {
        bArray = sqlAnswer.value(0).toByteArray();
        pixmap.loadFromData(bArray);
    }
    return pixmap;
}

void HistoryKeeper::saveAvatarHash(const QByteArray& hash, const QString& ownerId)
{
    QSqlQuery query;
    query.prepare("UPDATE aliases SET av_hash=:hash WHERE user_id = (:id)");
    query.bindValue(":hash", QString(hash.toBase64()));
    query.bindValue(":id", ownerId.left(64));
    query.exec();
}

QByteArray HistoryKeeper::getAvatarHash(const QString& ownerId)
{
    QByteArray bArray;
    QSqlQuery sqlAnswer = db->exec(QString("SELECT avatar FROM aliases WHERE user_id= '%1'").arg(ownerId.left(64)));
    if (sqlAnswer.first())
    {
        bArray = sqlAnswer.value(0).toByteArray();
    }
    return bArray;
}
