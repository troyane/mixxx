#include <QtCore>
#include <QtGui>
#include <QtSql>
#include <QtDebug>

#include "trackcollection.h"

#include "defs.h"
#include "library/librarytablemodel.h"
#include "library/schemamanager.h"
#include "library/queryutil.h"
#include "soundsourceproxy.h"
#include "trackinfoobject.h"
#include "xmlparse.h"
#include "util/sleepableqthread.h"

#include "controlobjectthread.h"
#include "controlobject.h"
#include "configobject.h"

#define MAX_LAMBDA_COUNT 8
#define MAX_CHUNK_SIZE 50

TrackCollection::TrackCollection(ConfigObject<ConfigValue>* pConfig)
    : m_pConfig(pConfig),
      m_pDatabase(NULL),
      m_pPlaylistDao(NULL),
      m_pCrateDao(NULL),
      m_pCueDao(NULL),
      m_pAnalysisDao(NULL),
      m_pTrackDao(NULL),
      m_stop(false),
      m_semLambdasReadyToCall(0),
      m_semLambdasFree(MAX_LAMBDA_COUNT),
      m_pCOTPlaylistIsBusy(NULL),
      m_supportedFileExtensionsRegex(
          SoundSourceProxy::supportedFileExtensionsRegex(),
          Qt::CaseInsensitive),
      m_inCallSync(false) {
    DBG() << "TrackCollection constructor \tfrom thread id="
          << QThread::currentThreadId() << "name="
          << QThread::currentThread()->objectName();
}

TrackCollection::~TrackCollection() {
    DBG() << "~TrackCollection()";
    m_pTrackDao->finish();

    if (m_pDatabase->isOpen()) {
        // There should never be an outstanding transaction when this code is
        // called. If there is, it means we probably aren't committing a
        // transaction somewhere that should be.
        if (m_pDatabase->rollback()) {
            qDebug() << "ERROR: There was a transaction in progress on the main database connection while shutting down."
                     << "There is a logic error somewhere.";
        }
        m_pDatabase->close();
    } else {
        qDebug() << "ERROR: The main database connection was closed before TrackCollection closed it."
                 << "There is a logic error somewhere.";
    }
    delete m_pPlaylistDao;
    delete m_pCrateDao;
    delete m_pCueDao;
    delete m_pAnalysisDao;
    delete m_pTrackDao;
}

void TrackCollection::run() {
    QThread::currentThread()->setObjectName("TrackCollection");
    DBG() << "id=" << QThread::currentThreadId()
          << "name=" << QThread::currentThread()->objectName();

    qRegisterMetaType<QSet<int> >("QSet<int>");

    DBG() << "Initializing DAOs inside TrackCollection's thread";

    createAndPopulateDbConnection();

    m_pTrackDao->initialize();
    m_pPlaylistDao->initialize();
    m_pCrateDao->initialize();
    m_pCueDao->initialize();

    m_pCOTPlaylistIsBusy = new ControlObjectThread(ConfigKey("[Playlist]", "isBusy"));
    Q_ASSERT(m_pCOTPlaylistIsBusy!=NULL);

    emit(initialized()); // to notify that Daos can be used

    func lambda;
    // main TrackCollection's loop
    while (!m_stop) {
        if (!m_semLambdasReadyToCall.tryAcquire(1)) {
            // no Lambda available, so unlock GUI.
            m_pCOTPlaylistIsBusy->set(0.0);
            m_semLambdasReadyToCall.acquire(1); // Sleep until new Lambdas have arrived
        }
        // got lambda on queue (or needness to stop thread)
        if (m_stop) {
            DBG() << "Need to stop thread";
            break;
        }
        if (!m_lambdas.isEmpty()) {
            m_lambdasQueueMutex.lock();
            lambda = m_lambdas.dequeue();
            m_lambdasQueueMutex.unlock();
            lambda();
            m_semLambdasFree.release(1);
        }
    }
    deleteLater();
    DBG() << " ### Thread ended ###";
}

// callAsync can be called from any thread. Be careful: callAsync runs asynchonously.
//    @param: lambda function, string (for debug purposes).
//    Catched values in lambda must be guarantly alive until end of execution lambda
//    (if catched by value) or you can catch by value.
void TrackCollection::callAsync(func lambda, QString where) {
    qDebug() << "callAsync from" << where;
    if (lambda == NULL) return;
    const bool inMainThread = QThread::currentThread() == qApp->thread();
    if (inMainThread) {
        DBG() << "LOCKING";
        Q_ASSERT(m_pCOTPlaylistIsBusy!=NULL);
        // lock GUI elements by setting [playlist] "isBusy"
        m_pCOTPlaylistIsBusy->set(1.0);
    }
    addLambdaToQueue(lambda);
}

// callAsync can be called from any thread
//    @param: lambda function, string (for debug purposes).
void TrackCollection::callSync(func lambda, QString where) {
    qDebug() << "callSync BEGIN from"<<where;
//    if (m_inCallSync) {
//        Q_ASSERT(!m_inCallSync);
//    }
//    qDebug() << "callSync from" << where;
    m_inCallSync = true;
    if (lambda == NULL) return;

    const bool inMainThread = QThread::currentThread() == qApp->thread();
    if (inMainThread) {
        DBG() << "LOCKING";
        Q_ASSERT(m_pCOTPlaylistIsBusy!=NULL);
        // lock GUI elements by setting [playlist] "isBusy"
        m_pCOTPlaylistIsBusy->set(1.0);
    }
    QMutex mutex;
    mutex.lock();
    func lambdaWithMutex =  [&mutex, &lambda] (void) {
        lambda();
        mutex.unlock();
    };
    addLambdaToQueue(lambdaWithMutex);

    while (!mutex.tryLock(5)) {
        if (inMainThread)
            MainExecuter::getInstance().call();
        qApp->processEvents(QEventLoop::AllEvents);
        // DBG() << "Start animation";
        // animationIsShowed = true;
    }
    mutex.unlock(); // QMutexes should be always destroyed in unlocked state.
    if (inMainThread) {
        DBG() << "UNLOCKING";
        Q_ASSERT(m_pCOTPlaylistIsBusy!=NULL);
        // lock GUI elements by setting [playlist] "isBusy
        m_pCOTPlaylistIsBusy->set(0.0);
    }
    m_inCallSync = false;
    qDebug() << "callSync END from"<<where;
}

void TrackCollection::addLambdaToQueue(func lambda) {
    //TODO(tro) check lambda
    m_semLambdasFree.acquire(1); // we'll wait here if lambdas count in queue is greater then MAX_LAMBDA_COUNT
    m_lambdasQueueMutex.lock();
    m_lambdas.enqueue(lambda);
    m_lambdasQueueMutex.unlock();
    m_semLambdasReadyToCall.release(1);
}

void TrackCollection::stopThread() {
    DBG() << "Stopping thread";
    m_stop = true;
    m_semLambdasReadyToCall.release(1);
}

bool TrackCollection::checkForTables() {
    if (!m_pDatabase->open()) {
        MainExecuter::callSync([this](void) {
            QMessageBox::critical(0, tr("Cannot open database"),
                                  tr("Unable to establish a database connection.\n"
                                     "Mixxx requires Qt with SQLite support. Please read "
                                     "the Qt SQL driver documentation for information on how "
                                     "to build it.\n\n"
                                     "Click OK to exit."), QMessageBox::Ok);
        });
        return false;
    }

    bool checkResult = true;
    MainExecuter::callSync([this, &checkResult](void) {
        int requiredSchemaVersion = 20; // TODO(xxx) avoid constant 20
        QString schemaFilename = m_pConfig->getResourcePath();
        schemaFilename.append("schema.xml");
        QString okToExit = tr("Click OK to exit.");
        QString upgradeFailed = tr("Cannot upgrade database schema");
        QString upgradeToVersionFailed = tr("Unable to upgrade your database schema to version %1")
                .arg(QString::number(requiredSchemaVersion));
        int result = SchemaManager::upgradeToSchemaVersion(schemaFilename, *m_pDatabase, requiredSchemaVersion);
        if (result < 0) {
            if (result == -1) {
                QMessageBox::warning(0, upgradeFailed,
                                     upgradeToVersionFailed + "\n" +
                                     tr("Your %1 file may be outdated.").arg(schemaFilename) +
                                     "\n\n" + okToExit,
                                     QMessageBox::Ok);
            } else if (result == -2) {
                QMessageBox::warning(0, upgradeFailed,
                                     upgradeToVersionFailed + "\n" +
                                     tr("Your mixxxdb.sqlite file may be corrupt.") + "\n" +
                                     tr("Try renaming it and restarting Mixxx.") +
                                     "\n\n" + okToExit,
                                     QMessageBox::Ok);
            } else { // -3
                QMessageBox::warning(0, upgradeFailed,
                                     upgradeToVersionFailed + "\n" +
                                     tr("Your %1 file may be missing or invalid.").arg(schemaFilename) +
                                     "\n\n" + okToExit,
                                     QMessageBox::Ok);
            }
            checkResult = false;
        }

    });
    return checkResult;
}

QSqlDatabase& TrackCollection::getDatabase() {
    return *m_pDatabase;
}

// Do a non-recursive import of all the songs in a directory. Does NOT decend into subdirectories.
//    @param trackDao The track data access object which provides a connection to the database.
//    We use this parameter in order to make this function callable from separate threads. You need to use a different DB connection for each thread.
//    @return true if the scan completed without being cancelled. False if the scan was cancelled part-way through.
//
bool TrackCollection::importDirectory(const QString& directory, TrackDAO& trackDao,
                                      const QStringList& nameFilters,
                                      volatile bool* cancel, volatile bool* pause) {
    //qDebug() << "TrackCollection::importDirectory(" << directory<< ")";

    emit(startedLoading());
    // QFileInfoList files;

    //get a list of the contents of the directory and go through it.
    QDirIterator it(directory, nameFilters, QDir::Files | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        //If a flag was raised telling us to cancel the library scan then stop.
        if (*cancel) {
            return false;
        }
        if (*pause) {
            DBG() << "in Pause";
            while (*pause && !*cancel)
                msleep(20);
        }
        if (*cancel) {
            return false;
        }

//        this->callSync(
//                    [this, &it, &directory, &trackDao, &nameFilters, &pause ] (void) {

//            trackDao.addTracksPrepare(); ///////

//            QString absoluteFilePath = it.next();
            addTrackToChunk(it.next(), trackDao);

            // If the track is in the database, mark it as existing. This code gets exectuted
            // when other files in the same directory have changed (the directory hash has changed).
//            trackDao.markTrackLocationAsVerified(absoluteFilePath);

//            // If the file already exists in the database, continue and go on to
//            // the next file.

//            // If the file doesn't already exist in the database, then add
//            // it. If it does exist in the database, then it is either in the
//            // user's library OR the user has "removed" the track via
//            // "Right-Click -> Remove". These tracks stay in the library, but
//            // their mixxx_deleted column is 1.
//            if (!trackDao.trackExistsInDatabase(absoluteFilePath)) {
//                //qDebug() << "Loading" << it.fileName();
//                emit(progressLoading(it.fileName()));

//                TrackPointer pTrack = TrackPointer(new TrackInfoObject(
//                                                       absoluteFilePath), &QObject::deleteLater);
//                if (trackDao.addTracksAdd(pTrack.data(), false)) {
//                    // Successful added
//                    // signal the main instance of TrackDao, that there is a
//                    // new Track in the database
//                    m_trackDao->databaseTrackAdded(pTrack);
//                } else {
//                    qDebug() << "Track ("+absoluteFilePath+") could not be added";
//                }
//            }
//            m_trackDao->addTracksFinish();
//        }, __PRETTY_FUNCTION__);
    }
    emit(finishedLoading());
    return true;
}

CrateDAO& TrackCollection::getCrateDAO() {
    return *m_pCrateDao;
}

TrackDAO& TrackCollection::getTrackDAO() {
    return *m_pTrackDao;
}

PlaylistDAO& TrackCollection::getPlaylistDAO() {
    return *m_pPlaylistDao;
}

QSharedPointer<BaseTrackCache> TrackCollection::getTrackSource(
        const QString& name) {
    return m_trackSources.value(name, QSharedPointer<BaseTrackCache>());
}

void TrackCollection::addTrackSource(
        const QString& name, QSharedPointer<BaseTrackCache> trackSource) {
    Q_ASSERT(!m_trackSources.contains(name));
    m_trackSources[name] = trackSource;
}

void TrackCollection::createAndPopulateDbConnection() {
    // initialize database connection in TrackCollection
    const QStringList avaiableDrivers = QSqlDatabase::drivers();
    const QString sqliteDriverName("QSQLITE");

    if (!avaiableDrivers.contains(sqliteDriverName)) {
        QString errorMsg = QString("No QSQLITE driver! Available QtSQL drivers: %1")
                .arg(avaiableDrivers.join(","));
        qDebug() << errorMsg;
        exit(-1);
    }

    m_pDatabase = new QSqlDatabase(QSqlDatabase::addDatabase("QSQLITE", "track_collection_connection"));
    m_pDatabase->setHostName("localhost");
    m_pDatabase->setDatabaseName(m_pConfig->getSettingsPath().append("/mixxxdb.sqlite"));
    m_pDatabase->setUserName("mixxx");
    m_pDatabase->setPassword("mixxx");
    bool ok = m_pDatabase->open();
    qDebug() << "DB status:" << m_pDatabase->databaseName() << "=" << ok;
    if (m_pDatabase->lastError().isValid()) {
        qDebug() << "Error loading database:" << m_pDatabase->lastError();
    }
    // Check for tables and create them if missing
    if (!checkForTables()) {
        // TODO(XXX) something a little more elegant
        exit(-1);
    }

    m_pPlaylistDao = new PlaylistDAO(*m_pDatabase);
    m_pCrateDao = new CrateDAO(*m_pDatabase);
    m_pCueDao = new CueDAO(*m_pDatabase);
    m_pAnalysisDao = new AnalysisDao(*m_pDatabase, m_pConfig);
    m_pTrackDao = new TrackDAO(*m_pDatabase, *m_pCueDao, *m_pPlaylistDao, *m_pCrateDao, *m_pAnalysisDao, m_pConfig);
}

void TrackCollection::addTrackToChunk(const QString filePath, TrackDAO& trackDao) {
    if (m_tracksListInCnunk.count() < MAX_CHUNK_SIZE) {
        m_tracksListInCnunk.append(filePath);
    } else {
        callSync( [this, &trackDao] (void) {
            addChunkToDatabase(trackDao);
        }, "addTrackToChunk");
    }
}

void TrackCollection::addChunkToDatabase(TrackDAO& trackDao) {
    DBG() << "Adding chunk to DB: " << m_tracksListInCnunk;
    trackDao.addTracksPrepare();
    foreach (QString trackPath, m_tracksListInCnunk) {
        // If the track is in the database, mark it as existing. This code gets exectuted
        // when other files in the same directory have changed (the directory hash has changed).
        trackDao.markTrackLocationAsVerified(trackPath);

        // If the file already exists in the database, continue and go on to
        // the next file.

        // If the file doesn't already exist in the database, then add
        // it. If it does exist in the database, then it is either in the
        // user's library OR the user has "removed" the track via
        // "Right-Click -> Remove". These tracks stay in the library, but
        // their mixxx_deleted column is 1.
        if (!trackDao.trackExistsInDatabase(trackPath)) {
            //qDebug() << "Loading" << it.fileName();
            emit(progressLoading(trackPath));

            TrackPointer pTrack = TrackPointer(new TrackInfoObject(trackPath), &QObject::deleteLater);
            if (trackDao.addTracksAdd(pTrack.data(), false)) {
                // Successful added
                // signal the main instance of TrackDao, that there is a
                // new Track in the database
                m_pTrackDao->databaseTrackAdded(pTrack);
            } else {
                qDebug() << "Track ("+trackPath+") could not be added";
            }
        }
    }
    trackDao.addTracksFinish();
    m_tracksListInCnunk.clear();
}
