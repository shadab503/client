/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <QFile>
#include <QStringList>
#include <QDebug>
#include <QElapsedTimer>
#include "ownsql.h"

#include <inttypes.h>

#include "syncjournaldb.h"
#include "syncjournalfilerecord.h"
#include "utility.h"
#include "version.h"
#include "filesystem.h"

#include "../../csync/src/std/c_jhash.h"

namespace OCC {

SyncJournalDb::SyncJournalDb(const QString& path, QObject *parent) :
    QObject(parent), _transaction(0), _possibleUpgradeFromMirall_1_5(false)
{

    _dbFile = path;
    if( !_dbFile.endsWith('/') ) {
        _dbFile.append('/');
    }
    _dbFile.append(".csync_journal.db");


}

bool SyncJournalDb::exists()
{
    QMutexLocker locker(&_mutex);
    return (!_dbFile.isEmpty() && QFile::exists(_dbFile));
}

QString SyncJournalDb::databaseFilePath()
{
    return _dbFile;
}

// Note that this does not change the size of the -wal file, but it is supposed to make
// the normal .db faster since the changes from the wal will be incorporated into it.
// Then the next sync (and the SocketAPI) will have a faster access.
void SyncJournalDb::walCheckpoint()
{
    QElapsedTimer t;
    t.start();
    SqlQuery pragma1(_db);
    pragma1.prepare("PRAGMA wal_checkpoint(FULL);");
    if (!pragma1.exec()) {
        qDebug() << pragma1.error();
    } else {
        qDebug() << Q_FUNC_INFO << "took" << t.elapsed() << "msec";
    }
}

void SyncJournalDb::startTransaction()
{
    if( _transaction == 0 ) {
        if( !_db.transaction() ) {
            qDebug() << "ERROR starting transaction: " << _db.error();
            return;
        }
        _transaction = 1;
        // qDebug() << "XXX Transaction start!";
    } else {
        qDebug() << "Database Transaction is running, do not starting another one!";
    }
}

void SyncJournalDb::commitTransaction()
{
    if( _transaction == 1 ) {
        if( ! _db.commit() ) {
            qDebug() << "ERROR committing to the database: " << _db.error();
            return;
        }
        _transaction = 0;
        // qDebug() << "XXX Transaction END!";
    } else {
        qDebug() << "No database Transaction to commit";
    }
}

bool SyncJournalDb::sqlFail( const QString& log, const SqlQuery& query )
{
    commitTransaction();
    qWarning() << "SQL Error" << log << query.error();
    Q_ASSERT(!"SQL ERROR");
    _db.close();
    return false;
}

bool SyncJournalDb::checkConnect()
{
    if( _db.isOpen() ) {
        return true;
    }

    if( _dbFile.isEmpty()) {
        qDebug() << "Database filename" + _dbFile + " is empty";
        return false;
    }

    bool isNewDb = !QFile::exists(_dbFile);

    // The database file is created by this call (SQLITE_OPEN_CREATE)
    if( !_db.open(_dbFile) ) {
        QString error = _db.error();
        qDebug() << "Error opening the db: " << error;
        return false;
    }

    if( !QFile::exists(_dbFile) ) {
        qDebug() << "Database file" + _dbFile + " does not exist";
        return false;
    }

    SqlQuery pragma1(_db);
    pragma1.prepare("SELECT sqlite_version();");
    if (!pragma1.exec()) {
        return sqlFail("SELECT sqlite_version()", pragma1);
    } else {
        pragma1.next();
        qDebug() << "sqlite3 version" << pragma1.stringValue(0);
    }

    pragma1.prepare("PRAGMA journal_mode=WAL;");
    if (!pragma1.exec()) {
        return sqlFail("Set PRAGMA synchronous", pragma1);
    } else {
        pragma1.next();
        qDebug() << "sqlite3 journal_mode=" << pragma1.stringValue(0);

    }

    pragma1.prepare("PRAGMA synchronous = 1;");
    if (!pragma1.exec()) {
        return sqlFail("Set PRAGMA synchronous", pragma1);
    }
    pragma1.prepare("PRAGMA case_sensitive_like = ON;");
    if (!pragma1.exec()) {
        return sqlFail("Set PRAGMA case_sensitivity", pragma1);
    }

    // Hide 'em all!
    FileSystem::setFileHidden(databaseFilePath(), true);
    FileSystem::setFileHidden(databaseFilePath() + "-wal", true);
    FileSystem::setFileHidden(databaseFilePath() + "-shm", true);

    /* Because insert are so slow, e do everything in a transaction, and one need to call commit */
    startTransaction();

    SqlQuery createQuery(_db);
    createQuery.prepare("CREATE TABLE IF NOT EXISTS metadata("
                         "phash INTEGER(8),"
                         "pathlen INTEGER,"
                         "path VARCHAR(4096),"
                         "inode INTEGER,"
                         "uid INTEGER,"
                         "gid INTEGER,"
                         "mode INTEGER,"
                         "modtime INTEGER(8),"
                         "type INTEGER,"
                         "md5 VARCHAR(32)," /* This is the etag.  Called md5 for compatibility */
                        // updateDatabaseStructure() will add a fileid column
                        // updateDatabaseStructure() will add a remotePerm column
                         "PRIMARY KEY(phash)"
                         ");");

    if (!createQuery.exec()) {
        return sqlFail("Create table metadata", createQuery);
    }

    createQuery.prepare("CREATE TABLE IF NOT EXISTS downloadinfo("
                         "path VARCHAR(4096),"
                         "tmpfile VARCHAR(4096),"
                         "etag VARCHAR(32),"
                         "errorcount INTEGER,"
                         "PRIMARY KEY(path)"
                         ");");

    if (!createQuery.exec()) {
        return sqlFail("Create table downloadinfo", createQuery);
    }

    createQuery.prepare("CREATE TABLE IF NOT EXISTS uploadinfo("
                           "path VARCHAR(4096),"
                           "chunk INTEGER,"
                           "transferid INTEGER,"
                           "errorcount INTEGER,"
                           "size INTEGER(8),"
                           "modtime INTEGER(8),"
                           "PRIMARY KEY(path)"
                           ");");

    if (!createQuery.exec()) {
        return sqlFail("Create table uploadinfo", createQuery);
    }

    // create the blacklist table.
    createQuery.prepare("CREATE TABLE IF NOT EXISTS blacklist ("
                        "path VARCHAR(4096),"
                        "lastTryEtag VARCHAR[32],"
                        "lastTryModtime INTEGER[8],"
                        "retrycount INTEGER,"
                        "errorstring VARCHAR[4096],"
                        "PRIMARY KEY(path)"
                        ");");

    if (!createQuery.exec()) {
        return sqlFail("Create table blacklist", createQuery);
    }

    createQuery.prepare("CREATE TABLE IF NOT EXISTS poll("
                           "path VARCHAR(4096),"
                           "modtime INTEGER(8),"
                           "pollpath VARCHAR(4096));");
    if (!createQuery.exec()) {
        return sqlFail("Create table poll", createQuery);
    }


    createQuery.prepare("CREATE TABLE IF NOT EXISTS version("
                               "major INTEGER(8),"
                               "minor INTEGER(8),"
                               "patch INTEGER(8),"
                               "custom VARCHAR(256)"
                               ");");
    if (!createQuery.exec()) {
        return sqlFail("Create table version", createQuery);
    }

    _possibleUpgradeFromMirall_1_5 = false;
    SqlQuery versionQuery("SELECT major, minor, patch FROM version;", _db);
    if (!versionQuery.next()) {
        // If there was no entry in the table, it means we are likely upgrading from 1.5
        if (!isNewDb) {
            qDebug() << Q_FUNC_INFO << "_possibleUpgradeFromMirall_1_5 detected!";
            _possibleUpgradeFromMirall_1_5 = true;
        }
        createQuery.prepare("INSERT INTO version VALUES (?1, ?2, ?3, ?4);");
        createQuery.bindValue(1, MIRALL_VERSION_MAJOR);
        createQuery.bindValue(2, MIRALL_VERSION_MINOR);
        createQuery.bindValue(3, MIRALL_VERSION_PATCH);
        createQuery.bindValue(3, MIRALL_VERSION_BUILD);
        createQuery.exec();

    } else {
        int major = versionQuery.intValue(0);
        int minor = versionQuery.intValue(1);
        int patch = versionQuery.intValue(2);

        // Not comparing the BUILD id here, correct?
        if( !(major == MIRALL_VERSION_MAJOR && minor == MIRALL_VERSION_MINOR && patch == MIRALL_VERSION_PATCH) ) {
            createQuery.prepare("UPDATE version SET major=?1, minor=?2, patch =?3, custom=?4 "
                                "WHERE major=?5 AND minor=?6 AND patch=?7;");
            createQuery.bindValue(1, MIRALL_VERSION_MAJOR);
            createQuery.bindValue(2, MIRALL_VERSION_MINOR);
            createQuery.bindValue(3, MIRALL_VERSION_PATCH);
            createQuery.bindValue(4, MIRALL_VERSION_BUILD);
            createQuery.bindValue(5, major);
            createQuery.bindValue(6, minor);
            createQuery.bindValue(7, patch);
            if (!createQuery.exec()) {
                return sqlFail("Update version", createQuery);
            }

        }
    }

    commitInternal("checkConnect");

    bool rc = updateDatabaseStructure();
    if( !rc ) {
        qDebug() << "WARN: Failed to update the database structure!";
    }

    _getFileRecordQuery.reset(new SqlQuery(_db));
    _getFileRecordQuery->prepare("SELECT path, inode, uid, gid, mode, modtime, type, md5, fileid, remotePerm, filesize FROM "
                                 "metadata WHERE phash=?1" );

    _setFileRecordQuery.reset(new SqlQuery(_db) );
    _setFileRecordQuery->prepare("INSERT OR REPLACE INTO metadata "
                                 "(phash, pathlen, path, inode, uid, gid, mode, modtime, type, md5, fileid, remotePerm, filesize) "
                                 "VALUES (?1 , ?2, ?3 , ?4 , ?5 , ?6 , ?7,  ?8 , ?9 , ?10, ?11, ?12, ?13);" );

    _getDownloadInfoQuery.reset(new SqlQuery(_db) );
    _getDownloadInfoQuery->prepare( "SELECT tmpfile, etag, errorcount FROM "
                                    "downloadinfo WHERE path=?1" );

    _setDownloadInfoQuery.reset(new SqlQuery(_db) );
    _setDownloadInfoQuery->prepare( "INSERT OR REPLACE INTO downloadinfo "
                                    "(path, tmpfile, etag, errorcount) "
                                    "VALUES ( ?1 , ?2, ?3, ?4 )" );

    _deleteDownloadInfoQuery.reset(new SqlQuery(_db) );
    _deleteDownloadInfoQuery->prepare( "DELETE FROM downloadinfo WHERE path=?1" );

    _getUploadInfoQuery.reset(new SqlQuery(_db));
    _getUploadInfoQuery->prepare( "SELECT chunk, transferid, errorcount, size, modtime FROM "
                                  "uploadinfo WHERE path=?1" );

    _setUploadInfoQuery.reset(new SqlQuery(_db));
    _setUploadInfoQuery->prepare( "INSERT OR REPLACE INTO uploadinfo "
                                  "(path, chunk, transferid, errorcount, size, modtime) "
                                  "VALUES ( ?1 , ?2, ?3 , ?4 ,  ?5, ?6 )");

    _deleteUploadInfoQuery.reset(new SqlQuery(_db));
    _deleteUploadInfoQuery->prepare("DELETE FROM uploadinfo WHERE path=?1" );


    _deleteFileRecordPhash.reset(new SqlQuery(_db));
    _deleteFileRecordPhash->prepare("DELETE FROM metadata WHERE phash=?1");

    _deleteFileRecordRecursively.reset(new SqlQuery(_db));
    _deleteFileRecordRecursively->prepare("DELETE FROM metadata WHERE path LIKE(?||'/%')");

    QString sql( "SELECT lastTryEtag, lastTryModtime, retrycount, errorstring, lastTryTime, ignoreDuration "
                 "FROM blacklist WHERE path=?1");
    if( Utility::fsCasePreserving() ) {
        // if the file system is case preserving we have to check the blacklist
        // case insensitively
        sql += QLatin1String(" COLLATE NOCASE");
    }
    _getBlacklistQuery.reset(new SqlQuery(_db));
    _getBlacklistQuery->prepare(sql);

    _setBlacklistQuery.reset(new SqlQuery(_db));
    _setBlacklistQuery->prepare("INSERT OR REPLACE INTO blacklist "
                                "(path, lastTryEtag, lastTryModtime, retrycount, errorstring, lastTryTime, ignoreDuration) "
                                "VALUES ( ?1, ?2, ?3, ?4, ?5, ?6, ?7)");

    // don't start a new transaction now
    commitInternal(QString("checkConnect End"), false);

    return rc;
}

void SyncJournalDb::close()
{
    QMutexLocker locker(&_mutex);
    qDebug() << Q_FUNC_INFO << _dbFile;

    commitTransaction();

    _getFileRecordQuery.reset(0);
    _setFileRecordQuery.reset(0);
    _getDownloadInfoQuery.reset(0);
    _setDownloadInfoQuery.reset(0);
    _deleteDownloadInfoQuery.reset(0);
    _getUploadInfoQuery.reset(0);
    _setUploadInfoQuery.reset(0);
    _deleteUploadInfoQuery.reset(0);
    _deleteFileRecordPhash.reset(0);
    _deleteFileRecordRecursively.reset(0);
    _getBlacklistQuery.reset(0);
    _possibleUpgradeFromMirall_1_5 = false;

    _db.close();
    _avoidReadFromDbOnNextSyncFilter.clear();
}


bool SyncJournalDb::updateDatabaseStructure()
{
    if (!updateMetadataTableStructure())
        return false;
    if (!updateBlacklistTableStructure())
        return false;
    return true;
}

bool SyncJournalDb::updateMetadataTableStructure()
{
    QStringList columns = tableColumns("metadata");
    bool re = true;

    // check if the file_id column is there and create it if not
    if( !checkConnect() ) {
        return false;
    }

    if( columns.indexOf(QLatin1String("fileid")) == -1 ) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE metadata ADD COLUMN fileid VARCHAR(128);");
        if( !query.exec() ) {
            sqlFail("updateMetadataTableStructure: Add column fileid", query);
            re = false;
        }

        query.prepare("CREATE INDEX metadata_file_id ON metadata(fileid);");
        if( ! query.exec() ) {
            sqlFail("updateMetadataTableStructure: create index fileid", query);
            re = false;
        }
        commitInternal("update database structure: add fileid col");
    }
    if( columns.indexOf(QLatin1String("remotePerm")) == -1 ) {

        SqlQuery query(_db);
        query.prepare("ALTER TABLE metadata ADD COLUMN remotePerm VARCHAR(128);");
        if( !query.exec()) {
            sqlFail("updateMetadataTableStructure: add column remotePerm", query);
            re = false;
        }
        commitInternal("update database structure (remotePerm)");
    }
    if( columns.indexOf(QLatin1String("filesize")) == -1 )
    {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE metadata ADD COLUMN filesize BIGINT;");
        if( !query.exec()) {
            sqlFail("updateDatabaseStructure: add column filesize", query);
            re = false;
        }
        commitInternal("update database structure: add filesize col");
    }

    if( 1 ) {
        SqlQuery query(_db);
        query.prepare("CREATE INDEX IF NOT EXISTS metadata_inode ON metadata(inode);");
        if( !query.exec()) {
            sqlFail("updateMetadataTableStructure: create index inode", query);
            re = false;
        }
        commitInternal("update database structure: add inode index");

    }

    if( 1 ) {
        SqlQuery query(_db);
        query.prepare("CREATE INDEX IF NOT EXISTS metadata_pathlen ON metadata(pathlen);");
        if( !query.exec()) {
            sqlFail("updateMetadataTableStructure: create index pathlen", query);
            re = false;
        }
        commitInternal("update database structure: add pathlen index");

    }
    return re;
}

bool SyncJournalDb::updateBlacklistTableStructure()
{
    QStringList columns = tableColumns("blacklist");
    bool re = true;

    // check if the file_id column is there and create it if not
    if( !checkConnect() ) {
        return false;
    }

    if( columns.indexOf(QLatin1String("lastTryTime")) == -1 ) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE blacklist ADD COLUMN lastTryTime INTEGER(8);");
        if( !query.exec() ) {
            sqlFail("updateBlacklistTableStructure: Add lastTryTime fileid", query);
            re = false;
        }
        query.prepare("ALTER TABLE blacklist ADD COLUMN ignoreDuration INTEGER(8);");
        if( !query.exec() ) {
            sqlFail("updateBlacklistTableStructure: Add ignoreDuration fileid", query);
            re = false;
        }
        commitInternal("update database structure: add lastTryTime, ignoreDuration cols");
    }

    return re;
}

QStringList SyncJournalDb::tableColumns( const QString& table )
{
    QStringList columns;
    if( !table.isEmpty() ) {

        if( checkConnect() ) {
            QString q = QString("PRAGMA table_info('%1');").arg(table);
            SqlQuery query(_db);
            query.prepare(q);

            if(!query.exec()) {
                QString err = query.error();
                qDebug() << "Error creating prepared statement: " << query.lastQuery() << ", Error:" << err;;
                return columns;
            }

            while( query.next() ) {
                columns.append( query.stringValue(1) );
            }
        }
    }
    qDebug() << "Columns in the current journal: " << columns;

    return columns;
}

qint64 SyncJournalDb::getPHash(const QString& file)
{
    QByteArray utf8File = file.toUtf8();
    int64_t h;

    if( file.isEmpty() ) {
        return -1;
    }

    int len = utf8File.length();

    h = c_jhash64((uint8_t *) utf8File.data(), len, 0);
    return h;
}

bool SyncJournalDb::setFileRecord( const SyncJournalFileRecord& _record )
{
    SyncJournalFileRecord record = _record;
    QMutexLocker locker(&_mutex);

    if (!_avoidReadFromDbOnNextSyncFilter.isEmpty()) {
        // If we are a directory that should not be read from db next time, don't write the etag
        QString prefix = record._path + "/";
        foreach(const QString &it, _avoidReadFromDbOnNextSyncFilter) {
            if (it.startsWith(prefix)) {
                qDebug() << "Filtered writing the etag of" << prefix << "because it is a prefix of" << it;
                record._etag = "_invalid_";
                break;
            }
        }
    }

    qlonglong phash = getPHash(record._path);
    if( checkConnect() ) {
        QByteArray arr = record._path.toUtf8();
        int plen = arr.length();

        QString etag( record._etag );
        if( etag.isEmpty() ) etag = "";
        QString fileId( record._fileId);
        if( fileId.isEmpty() ) fileId = "";
        QString remotePerm (record._remotePerm);
        if (remotePerm.isEmpty()) remotePerm = QString(); // have NULL in DB (vs empty)
        _setFileRecordQuery->reset();
        _setFileRecordQuery->bindValue(1, QString::number(phash));
        _setFileRecordQuery->bindValue(2, plen);
        _setFileRecordQuery->bindValue(3, record._path );
        _setFileRecordQuery->bindValue(4, record._inode );
        _setFileRecordQuery->bindValue(5, 0 ); // uid Not used
        _setFileRecordQuery->bindValue(6, 0 ); // gid Not used
        _setFileRecordQuery->bindValue(7, record._mode );
        _setFileRecordQuery->bindValue(8, QString::number(Utility::qDateTimeToTime_t(record._modtime)));
        _setFileRecordQuery->bindValue(9, QString::number(record._type) );
        _setFileRecordQuery->bindValue(10, etag );
        _setFileRecordQuery->bindValue(11, fileId );
        _setFileRecordQuery->bindValue(12, remotePerm );
        _setFileRecordQuery->bindValue(13, record._fileSize );

        if( !_setFileRecordQuery->exec() ) {
            qWarning() << "Error SQL statement setFileRecord: " << _setFileRecordQuery->lastQuery() <<  " :"
                       << _setFileRecordQuery->error();
            return false;
        }

        qDebug() <<  _setFileRecordQuery->lastQuery() << phash << plen << record._path << record._inode
                 << record._mode
                 << QString::number(Utility::qDateTimeToTime_t(record._modtime)) << QString::number(record._type)
                 << record._etag << record._fileId << record._remotePerm << record._fileSize;

        _setFileRecordQuery->reset();
        return true;
    } else {
        qDebug() << "Failed to connect database.";
        return false; // checkConnect failed.
    }
}

bool SyncJournalDb::deleteFileRecord(const QString& filename, bool recursively)
{
    QMutexLocker locker(&_mutex);

    if( checkConnect() ) {
        // if (!recursively) {
        // always delete the actual file.

        qlonglong phash = getPHash(filename);
        _deleteFileRecordPhash->reset();
        _deleteFileRecordPhash->bindValue( 1, QString::number(phash) );

        if( !_deleteFileRecordPhash->exec() ) {
            qWarning() << "Exec error of SQL statement: "
                       << _deleteFileRecordPhash->lastQuery()
                       <<  " : " << _deleteFileRecordPhash->error();
            return false;
        }
        qDebug() <<  _deleteFileRecordPhash->lastQuery() << phash << filename;
        _deleteFileRecordPhash->reset();
        if( recursively) {
            _deleteFileRecordRecursively->reset();
            _deleteFileRecordRecursively->bindValue(1, filename);
            if( !_deleteFileRecordRecursively->exec() ) {
                qWarning() << "Exec error of SQL statement: "
                           << _deleteFileRecordRecursively->lastQuery()
                           <<  " : " << _deleteFileRecordRecursively->error();
                return false;
            }
            qDebug() <<  _deleteFileRecordRecursively->lastQuery()  << filename;
            _deleteFileRecordRecursively->reset();
        }
        return true;
    } else {
        qDebug() << "Failed to connect database.";
        return false; // checkConnect failed.
    }
}


SyncJournalFileRecord SyncJournalDb::getFileRecord( const QString& filename )
{
    QMutexLocker locker(&_mutex);

    qlonglong phash = getPHash( filename );
    SyncJournalFileRecord rec;

    if( checkConnect() ) {
        _getFileRecordQuery->reset();
        _getFileRecordQuery->bindValue(1, QString::number(phash));

        if (!_getFileRecordQuery->exec()) {
            QString err = _getFileRecordQuery->error();
            qDebug() << "Error creating prepared statement: " << _getFileRecordQuery->lastQuery() << ", Error:" << err;;
            return rec;
        }

        if( _getFileRecordQuery->next() ) {
            rec._path    = _getFileRecordQuery->stringValue(0);
            rec._inode   = _getFileRecordQuery->intValue(1);
            //rec._uid     = _getFileRecordQuery->value(2).toInt(&ok); Not Used
            //rec._gid     = _getFileRecordQuery->value(3).toInt(&ok); Not Used
            rec._mode    = _getFileRecordQuery->intValue(4);
            rec._modtime = Utility::qDateTimeFromTime_t(_getFileRecordQuery->int64Value(5));
            rec._type    = _getFileRecordQuery->intValue(6);
            rec._etag    = _getFileRecordQuery->baValue(7);
            rec._fileId  = _getFileRecordQuery->baValue(8);
            rec._remotePerm = _getFileRecordQuery->baValue(9);
            rec._fileSize   = _getFileRecordQuery->int64Value(10);
        } else {
            QString err = _getFileRecordQuery->error();
            qDebug() << "No journal entry found for " << filename;
        }
        _getFileRecordQuery->reset();
    }
    return rec;
}

bool SyncJournalDb::postSyncCleanup(const QSet<QString> &items )
{
    QMutexLocker locker(&_mutex);

    if( !checkConnect() ) {
        return false;
    }

    SqlQuery query(_db);
    query.prepare("SELECT phash, path FROM metadata order by path");

    if (!query.exec()) {
        QString err = query.error();
        qDebug() << "Error creating prepared statement: " << query.lastQuery() << ", Error:" << err;;
        return false;
    }

    QStringList superfluousItems;

    while(query.next()) {
        const QString file = query.stringValue(1);
        bool contained = items.contains(file);
        if( !contained ) {
            superfluousItems.append(query.stringValue(0));
        }
    }

    if( superfluousItems.count() )  {
        QString sql = "DELETE FROM metadata WHERE phash in ("+ superfluousItems.join(",")+")";
        qDebug() << "Sync Journal cleanup: " << sql;
        SqlQuery delQuery(_db);
        delQuery.prepare(sql);
        if( !delQuery.exec() ) {
            QString err = delQuery.error();
            qDebug() << "Error removing superfluous journal entries: " << delQuery.lastQuery() << ", Error:" << err;;
            return false;
        }
    }

    // Incoroporate results back into main DB
    walCheckpoint();

    if (_possibleUpgradeFromMirall_1_5) {
        _possibleUpgradeFromMirall_1_5 = false; // should be handled now
    }

    return true;
}

int SyncJournalDb::getFileRecordCount()
{
    QMutexLocker locker(&_mutex);

    if( !checkConnect() ) {
        return -1;
    }

    SqlQuery query(_db);
    query.prepare("SELECT COUNT(*) FROM metadata");

    if (!query.exec()) {
        QString err = query.error();
        qDebug() << "Error creating prepared statement: " << query.lastQuery() << ", Error:" << err;;
        return 0;
    }

    if (query.next()) {
        int count = query.intValue(0);
        return count;
    }

    return 0;
}

static void toDownloadInfo(SqlQuery &query, SyncJournalDb::DownloadInfo * res)
{
    bool ok = true;
    res->_tmpfile    = query.stringValue(0);
    res->_etag       = query.baValue(1);
    res->_errorCount = query.intValue(2);
    res->_valid      = ok;
}

static bool deleteBatch(SqlQuery & query, const QStringList & entries, const QString & name)
{
    if (entries.isEmpty())
        return true;

    qDebug() << "Removing stale " << qPrintable(name) << " entries: " << entries.join(", ");
    // FIXME: Was ported from execBatch, check if correct!
    foreach( const QString& entry, entries ) {
        query.reset();
        query.bindValue(1, entry);
        if (!query.exec()) {
            QString err = query.error();
            qDebug() << "Error removing stale " << qPrintable(name) << " entries: "
                     << query.lastQuery() << ", Error:" << err;
            return false;
        }
    }
    query.reset(); // viel hilft viel ;-)

    return true;
}

SyncJournalDb::DownloadInfo SyncJournalDb::getDownloadInfo(const QString& file)
{
    QMutexLocker locker(&_mutex);

    DownloadInfo res;

    if( checkConnect() ) {
        _getDownloadInfoQuery->reset();
        _getDownloadInfoQuery->bindValue(1, file);

        if (!_getDownloadInfoQuery->exec()) {
            QString err = _getDownloadInfoQuery->error();
            qDebug() << "Database error for file " << file << " : " << _getDownloadInfoQuery->lastQuery() << ", Error:" << err;;
            return res;
        }

        if( _getDownloadInfoQuery->next() ) {
            toDownloadInfo(*_getDownloadInfoQuery, &res);
        } else {
            res._valid = false;
        }
        _getDownloadInfoQuery->reset();
    }
    return res;
}

void SyncJournalDb::setDownloadInfo(const QString& file, const SyncJournalDb::DownloadInfo& i)
{
    QMutexLocker locker(&_mutex);

    if( !checkConnect() ) {
        return;
    }

    if (i._valid) {
        _setDownloadInfoQuery->reset();
        _setDownloadInfoQuery->bindValue(1, file);
        _setDownloadInfoQuery->bindValue(2, i._tmpfile);
        _setDownloadInfoQuery->bindValue(3, i._etag );
        _setDownloadInfoQuery->bindValue(4, i._errorCount );

        if( !_setDownloadInfoQuery->exec() ) {
            qWarning() << "Exec error of SQL statement: " << _setDownloadInfoQuery->lastQuery() <<  " :"   << _setDownloadInfoQuery->error();
            return;
        }

        qDebug() <<  _setDownloadInfoQuery->lastQuery() << file << i._tmpfile << i._etag << i._errorCount;
        _setDownloadInfoQuery->reset();

    } else {
        _deleteDownloadInfoQuery->reset();
        _deleteDownloadInfoQuery->bindValue( 1, file );

        if( !_deleteDownloadInfoQuery->exec() ) {
            qWarning() << "Exec error of SQL statement: " << _deleteDownloadInfoQuery->lastQuery() <<  " : " << _deleteDownloadInfoQuery->error();
            return;
        }
        qDebug() <<  _deleteDownloadInfoQuery->lastQuery()  << file;
        _deleteDownloadInfoQuery->reset();
    }
}

QVector<SyncJournalDb::DownloadInfo> SyncJournalDb::getAndDeleteStaleDownloadInfos(const QSet<QString>& keep)
{
    QVector<SyncJournalDb::DownloadInfo> empty_result;
    QMutexLocker locker(&_mutex);

    if (!checkConnect()) {
        return empty_result;
    }

    SqlQuery query(_db);
    // The selected values *must* match the ones expected by toDownloadInfo().
    query.prepare("SELECT tmpfile, etag, errorcount, path FROM downloadinfo");

    if (!query.exec()) {
        QString err = query.error();
        qDebug() << "Error creating prepared statement: " << query.lastQuery() << ", Error:" << err;
        return empty_result;
    }

    QStringList superfluousPaths;
    QVector<SyncJournalDb::DownloadInfo> deleted_entries;

    while (query.next()) {
        const QString file = query.stringValue(3); // path
        if (!keep.contains(file)) {
            superfluousPaths.append(file);
            DownloadInfo info;
            toDownloadInfo(query, &info);
            deleted_entries.append(info);
        }
    }

    if (!deleteBatch(*_deleteDownloadInfoQuery, superfluousPaths, "downloadinfo"))
        return empty_result;

    return deleted_entries;
}

int SyncJournalDb::downloadInfoCount()
{
    int re = 0;

    QMutexLocker locker(&_mutex);
    if( checkConnect() ) {
        SqlQuery query("SELECT count(*) FROM downloadinfo", _db);

        if( ! query.exec() ) {
            sqlFail("Count number of downloadinfo entries failed", query);
        }
        if( query.next() ) {
            re = query.intValue(0);
        }
    }
    return re;
}

SyncJournalDb::UploadInfo SyncJournalDb::getUploadInfo(const QString& file)
{
    QMutexLocker locker(&_mutex);

    UploadInfo res;

    if( checkConnect() ) {

        _getUploadInfoQuery->reset();
        _getUploadInfoQuery->bindValue(1, file);

        if (!_getUploadInfoQuery->exec()) {
            QString err = _getUploadInfoQuery->error();
            qDebug() << "Database error for file " << file << " : " << _getUploadInfoQuery->lastQuery() << ", Error:" << err;
            return res;
        }

        if( _getUploadInfoQuery->next() ) {
            bool ok = true;
            res._chunk      = _getUploadInfoQuery->intValue(0);
            res._transferid = _getUploadInfoQuery->intValue(1);
            res._errorCount = _getUploadInfoQuery->intValue(2);
            res._size       = _getUploadInfoQuery->int64Value(3);
            res._modtime    = Utility::qDateTimeFromTime_t(_getUploadInfoQuery->int64Value(4));
            res._valid      = ok;
        }
        _getUploadInfoQuery->reset();
    }
    return res;
}

void SyncJournalDb::setUploadInfo(const QString& file, const SyncJournalDb::UploadInfo& i)
{
    QMutexLocker locker(&_mutex);

    if( !checkConnect() ) {
        return;
    }

    if (i._valid) {
        _setUploadInfoQuery->reset();
        _setUploadInfoQuery->bindValue(1, file);
        _setUploadInfoQuery->bindValue(2, i._chunk);
        _setUploadInfoQuery->bindValue(3, i._transferid );
        _setUploadInfoQuery->bindValue(4, i._errorCount );
        _setUploadInfoQuery->bindValue(5, i._size );
        _setUploadInfoQuery->bindValue(6, Utility::qDateTimeToTime_t(i._modtime) );

        if( !_setUploadInfoQuery->exec() ) {
            qWarning() << "Exec error of SQL statement: " << _setUploadInfoQuery->lastQuery() <<  " :"   << _setUploadInfoQuery->error();
            return;
        }

        qDebug() <<  _setUploadInfoQuery->lastQuery() << file << i._chunk << i._transferid << i._errorCount;
        _setUploadInfoQuery->reset();
    } else {
        _deleteUploadInfoQuery->reset();
        _deleteUploadInfoQuery->bindValue(1, file);

        if( !_deleteUploadInfoQuery->exec() ) {
            qWarning() << "Exec error of SQL statement: " << _deleteUploadInfoQuery->lastQuery() <<  " : " << _deleteUploadInfoQuery->error();
            return;
        }
        qDebug() <<  _deleteUploadInfoQuery->lastQuery() << file;
        _deleteUploadInfoQuery->reset();
    }
}

bool SyncJournalDb::deleteStaleUploadInfos(const QSet<QString> &keep)
{
    QMutexLocker locker(&_mutex);

    if (!checkConnect()) {
        return false;
    }

    SqlQuery query(_db);
    query.prepare("SELECT path FROM uploadinfo");

    if (!query.exec()) {
        QString err = query.error();
        qDebug() << "Error creating prepared statement: " << query.lastQuery() << ", Error:" << err;
        return false;
    }

    QStringList superfluousPaths;

    while (query.next()) {
        const QString file = query.stringValue(0);
        if (!keep.contains(file)) {
            superfluousPaths.append(file);
        }
    }

    return deleteBatch(*_deleteUploadInfoQuery, superfluousPaths, "uploadinfo");
}

SyncJournalBlacklistRecord SyncJournalDb::blacklistEntry( const QString& file )
{
    QMutexLocker locker(&_mutex);
    SyncJournalBlacklistRecord entry;

    if( file.isEmpty() ) return entry;

    // SELECT lastTryEtag, lastTryModtime, retrycount, errorstring

    if( checkConnect() ) {
        _getBlacklistQuery->reset();
        _getBlacklistQuery->bindValue( 1, file );
        if( _getBlacklistQuery->exec() ){
            if( _getBlacklistQuery->next() ) {
                entry._lastTryEtag    = _getBlacklistQuery->baValue(0);
                entry._lastTryModtime = _getBlacklistQuery->int64Value(1);
                entry._retryCount     = _getBlacklistQuery->intValue(2);
                entry._errorString    = _getBlacklistQuery->stringValue(3);
                entry._lastTryTime    = _getBlacklistQuery->int64Value(4);
                entry._ignoreDuration = _getBlacklistQuery->int64Value(5);
                entry._file           = file;
            }
            _getBlacklistQuery->reset();
        } else {
            qWarning() << "Exec error blacklist: " << _getBlacklistQuery->lastQuery() <<  " : "
                       << _getBlacklistQuery->error();
        }
    }

    return entry;
}

bool SyncJournalDb::deleteStaleBlacklistEntries(const QSet<QString> &keep)
{
    QMutexLocker locker(&_mutex);

    if (!checkConnect()) {
        return false;
    }

    SqlQuery query(_db);
    query.prepare("SELECT path FROM blacklist");

    if (!query.exec()) {
        QString err = query.error();
        qDebug() << "Error creating prepared statement: " << query.lastQuery() << ", Error:" << err;
        return false;
    }

    QStringList superfluousPaths;

    while (query.next()) {
        const QString file = query.stringValue(0);
        if (!keep.contains(file)) {
            superfluousPaths.append(file);
        }
    }

    SqlQuery delQuery(_db);
    delQuery.prepare("DELETE FROM blacklist WHERE path = ?");
    return deleteBatch(delQuery, superfluousPaths, "blacklist");
}

int SyncJournalDb::blackListEntryCount()
{
    int re = 0;

    QMutexLocker locker(&_mutex);
    if( checkConnect() ) {
        SqlQuery query("SELECT count(*) FROM blacklist", _db);

        if( ! query.exec() ) {
            sqlFail("Count number of blacklist entries failed", query);
        }
        if( query.next() ) {
            re = query.intValue(0);
        }
    }
    return re;
}

int SyncJournalDb::wipeBlacklist()
{
    QMutexLocker locker(&_mutex);
    if( checkConnect() ) {
        SqlQuery query(_db);

        query.prepare("DELETE FROM blacklist");

        if( ! query.exec() ) {
            sqlFail("Deletion of whole blacklist failed", query);
            return -1;
        }
        return query.numRowsAffected();
    }
    return -1;
}

void SyncJournalDb::wipeBlacklistEntry( const QString& file )
{
    if( file.isEmpty() ) {
        return;
    }

    QMutexLocker locker(&_mutex);
    if( checkConnect() ) {
        SqlQuery query(_db);

        query.prepare("DELETE FROM blacklist WHERE path=?1");
        query.bindValue(1, file);
        if( ! query.exec() ) {
            sqlFail("Deletion of blacklist item failed.", query);
        }
    }
}

void SyncJournalDb::updateBlacklistEntry( const SyncJournalBlacklistRecord& item )
{
    if( !checkConnect() ) {
        return;
    }

    QMutexLocker locker(&_mutex);

    _setBlacklistQuery->bindValue(1, item._file);
    _setBlacklistQuery->bindValue(2, item._lastTryEtag);
    _setBlacklistQuery->bindValue(3, QString::number(item._lastTryModtime));
    _setBlacklistQuery->bindValue(4, item._retryCount);
    _setBlacklistQuery->bindValue(5, item._errorString);
    _setBlacklistQuery->bindValue(6, QString::number(item._lastTryTime));
    _setBlacklistQuery->bindValue(7, QString::number(item._ignoreDuration));
    if( !_setBlacklistQuery->exec() ) {
        QString bug = _setBlacklistQuery->error();
        qDebug() << "SQL exec blacklistitem insert or replace failed: "<< bug;
    }
    qDebug() << "set blacklist entry for " << item._file << item._retryCount
             << item._errorString << item._lastTryTime << item._ignoreDuration
             << item._lastTryModtime << item._lastTryEtag;
    _setBlacklistQuery->reset();

}

QVector< SyncJournalDb::PollInfo > SyncJournalDb::getPollInfos()
{
    QMutexLocker locker(&_mutex);

    QVector< SyncJournalDb::PollInfo > res;

    if( !checkConnect() )
        return res;

    SqlQuery query("SELECT path, modtime, pollpath FROM poll",_db);

    if (!query.exec()) {
        QString err = query.error();
        qDebug() << "Database error :" << query.lastQuery() << ", Error:" << err;
        return res;
    }

    while( query.next() ) {
        PollInfo info;
        info._file = query.stringValue(0);
        info._modtime = query.int64Value(1);
        info._url = query.stringValue(2);
        res.append(info);
    }

    query.finish();
    return res;
}

void SyncJournalDb::setPollInfo(const SyncJournalDb::PollInfo& info)
{
    QMutexLocker locker(&_mutex);
    if( !checkConnect() ) {
        return;
    }

    if (info._url.isEmpty()) {
        SqlQuery query("DELETE FROM poll WHERE path=?", _db);
        query.bindValue(0, info._file);
        if( !query.exec() ) {
            qDebug() << "SQL error in setPollInfo: "<< query.error();
        } else {
            qDebug() << query.lastQuery()  << info._file;
        }
    } else {
        SqlQuery query("INSERT OR REPLACE INTO poll (path, modtime, pollpath) VALUES( ? , ? , ? )", _db);
        query.bindValue(0, info._file);
        query.bindValue(1, QString::number(info._modtime));
        query.bindValue(2, info._url);
        if( !query.exec() ) {
            qDebug() << "SQL error in setPollInfo: "<< query.error();
        } else {
            qDebug() << query.lastQuery()  << info._file << info._url;
        }
    }
}

void SyncJournalDb::avoidRenamesOnNextSync(const QString& path)
{
    QMutexLocker locker(&_mutex);

    if( !checkConnect() ) {
        return;
    }

    SqlQuery query(_db);
    query.prepare("UPDATE metadata SET fileid = '', inode = '0' WHERE path == ?1 OR path LIKE(?2||'/%')");
    query.bindValue(1, path);
    query.bindValue(2, path);
    if( !query.exec() ) {
        qDebug() << Q_FUNC_INFO << "SQL error in avoidRenamesOnNextSync: "<< query.error();
    } else {
        qDebug() << Q_FUNC_INFO << query.lastQuery()  << path << "(" << query.numRowsAffected() << " rows)";
    }

    // We also need to remove the ETags so the update phase refreshes the directory paths
    // on the next sync
    locker.unlock();
    avoidReadFromDbOnNextSync(path);
}

void SyncJournalDb::avoidReadFromDbOnNextSync(const QString& fileName)
{
    //Make sure that on the next sync, filName is not read from the DB but use the PROPFIND to
    //get the info from the server
    // We achieve that by clearing the etag of the parents directory recursively

    QMutexLocker locker(&_mutex);

    if( !checkConnect() ) {
        return;
    }

    SqlQuery query(_db);
    // This query will match entries for whitch the path is a prefix of fileName
    query.prepare("UPDATE metadata SET md5='_invalid_' WHERE ?1 LIKE(path||'/%') AND type == 2;"); // CSYNC_FTW_TYPE_DIR == 2
    query.bindValue(1, fileName);
    if( !query.exec() ) {
        qDebug() << Q_FUNC_INFO << "SQL error in avoidRenamesOnNextSync: "<< query.error();
    } else {
        qDebug() << Q_FUNC_INFO << query.lastQuery()  << fileName << "(" << query.numRowsAffected() << " rows)";
    }

    // Prevent future overwrite of the etag for this sync
    _avoidReadFromDbOnNextSyncFilter.append(fileName);
}

void SyncJournalDb::commit(const QString& context, bool startTrans)
{
    QMutexLocker lock(&_mutex);
    commitInternal(context, startTrans);
}

void SyncJournalDb::commitIfNeededAndStartNewTransaction(const QString &context)
{
    QMutexLocker lock(&_mutex);
    if( _transaction == 1 ) {
        commitInternal(context, true);
    } else {
        startTransaction();
    }
}


void SyncJournalDb::commitInternal(const QString& context, bool startTrans )
{
    qDebug() << Q_FUNC_INFO << "Transaction commit " << context << (startTrans ? "and starting new transaction" : "");
    commitTransaction();

    if( startTrans ) {
        startTransaction();
    }
}

SyncJournalDb::~SyncJournalDb()
{
    close();
}

bool SyncJournalDb::isConnected()
{
    QMutexLocker lock(&_mutex);
    return checkConnect();
}

bool SyncJournalDb::isUpdateFrom_1_5()
{
    QMutexLocker lock(&_mutex);
    checkConnect();
    return _possibleUpgradeFromMirall_1_5;
}

bool operator==(const SyncJournalDb::DownloadInfo & lhs,
                const SyncJournalDb::DownloadInfo & rhs)
{
    return     lhs._errorCount == rhs._errorCount
            && lhs._etag == rhs._etag
            && lhs._tmpfile == rhs._tmpfile
            && lhs._valid == rhs._valid;

}

bool operator==(const SyncJournalDb::UploadInfo & lhs,
                const SyncJournalDb::UploadInfo & rhs)
{
    return     lhs._errorCount == rhs._errorCount
            && lhs._chunk == rhs._chunk
            && lhs._modtime == rhs._modtime
            && lhs._valid == rhs._valid
            && lhs._size == rhs._size
            && lhs._transferid == rhs._transferid;
}

} // namespace OCC
