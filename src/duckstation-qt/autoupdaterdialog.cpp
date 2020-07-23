#include "autoupdaterdialog.h"
#include "common/file_system.h"
#include "common/log.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include "scmversion/scmversion.h"
#include "unzip.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QProcess>
#include <QtCore/QString>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
Log_SetChannel(AutoUpdaterDialog);

static constexpr char LATEST_TAG_URL[] = "https://api.github.com/repos/stenzek/duckstation/tags";
static constexpr char LATEST_RELEASE_URL[] = "https://api.github.com/repos/stenzek/duckstation/releases/tags/latest";
static constexpr char UPDATE_ASSET_FILENAME[] = "duckstation-windows-x64-release.7z";

AutoUpdaterDialog::AutoUpdaterDialog(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QDialog(parent), m_host_interface(host_interface)
{
  m_network_access_mgr = new QNetworkAccessManager(this);

  m_ui.setupUi(this);

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  // m_ui.description->setTextInteractionFlags(Qt::TextBrowserInteraction);
  // m_ui.description->setOpenExternalLinks(true);

  connect(m_ui.downloadAndInstall, &QPushButton::clicked, this, &AutoUpdaterDialog::downloadUpdateClicked);
  connect(m_ui.skipThisUpdate, &QPushButton::clicked, this, &AutoUpdaterDialog::skipThisUpdateClicked);
  connect(m_ui.remindMeLater, &QPushButton::clicked, this, &AutoUpdaterDialog::remindMeLaterClicked);
}

AutoUpdaterDialog::~AutoUpdaterDialog() = default;

void AutoUpdaterDialog::queueUpdateCheck()
{
  connect(m_network_access_mgr, &QNetworkAccessManager::finished, this, &AutoUpdaterDialog::getLatestTagComplete);

  QUrl url(QUrl::fromEncoded(QByteArray(LATEST_TAG_URL, sizeof(LATEST_TAG_URL) - 1)));
  QNetworkRequest request(url);
  m_network_access_mgr->get(request);
}

void AutoUpdaterDialog::queueGetLatestRelease()
{
  connect(m_network_access_mgr, &QNetworkAccessManager::finished, this, &AutoUpdaterDialog::getLatestReleaseComplete);

  QUrl url(QUrl::fromEncoded(QByteArray(LATEST_RELEASE_URL, sizeof(LATEST_RELEASE_URL) - 1)));
  QNetworkRequest request(url);
  m_network_access_mgr->get(request);
}

void AutoUpdaterDialog::getLatestTagComplete(QNetworkReply* reply)
{
  m_network_access_mgr->disconnect(this);
  reply->deleteLater();

  if (reply->error() == QNetworkReply::NoError)
  {
    const QByteArray reply_json(reply->readAll());
    QJsonParseError parse_error;
    QJsonDocument doc(QJsonDocument::fromJson(reply_json, &parse_error));
    if (doc.isArray())
    {
      const QJsonArray doc_array(doc.array());
      for (const QJsonValue& val : doc_array)
      {
        if (!val.isObject())
          continue;

        if (val["name"].toString() != QStringLiteral("latest"))
          continue;

        m_latest_sha = val["commit"].toObject()["sha"].toString();
        if (m_latest_sha.isEmpty())
          continue;

        if (updateNeeded())
        {
          queueGetLatestRelease();
          return;
        }
      }

      Log_ErrorPrintf("latest release not found in JSON");
    }
    else
    {
      Log_ErrorPrintf("JSON is not an array");
    }
  }
  else
  {
    Log_ErrorPrintf("Failed to download latest tag info: %d", static_cast<int>(reply->error()));
  }

  emit updateCheckCompleted();
}

void AutoUpdaterDialog::getLatestReleaseComplete(QNetworkReply* reply)
{
  m_network_access_mgr->disconnect(this);
  reply->deleteLater();

  if (reply->error() == QNetworkReply::NoError)
  {
    const QByteArray reply_json(reply->readAll());
    QJsonParseError parse_error;
    QJsonDocument doc(QJsonDocument::fromJson(reply_json, &parse_error));
    if (doc.isObject())
    {
      const QJsonObject doc_object(doc.object());

      // search for the correct file
      const QJsonArray assets(doc_object["assets"].toArray());
      const QString asset_filename(UPDATE_ASSET_FILENAME);
      for (const QJsonValue& asset : assets)
      {
        const QJsonObject asset_obj(asset.toObject());
        if (asset_obj["name"] == asset_filename)
        {
          m_download_url = asset_obj["url"].toString();
          if (!m_download_url.isEmpty())
          {
            m_ui.currentVersion->setText(tr("Current Version: %1 (%2)").arg(g_scm_tag_str).arg(__TIMESTAMP__));
            m_ui.newVersion->setText(
              tr("New Version: %1 (%2)").arg(m_latest_sha).arg(doc_object["published_at"].toString()));
            m_ui.updateNotes->setText(doc_object["body"].toString());
            exec();

            emit updateCheckCompleted();
            return;
          }

          break;
        }
      }

      Log_ErrorPrintf("Asset/asset download not found");
    }
    else
    {
      Log_ErrorPrintf("JSON is not an object");
    }
  }
  else
  {
    Log_ErrorPrintf("Failed to download latest release info: %d", static_cast<int>(reply->error()));
  }

  emit updateCheckCompleted();
}

void AutoUpdaterDialog::downloadUpdateClicked()
{
  QUrl url(m_download_url);
  QNetworkRequest request(url);
  QNetworkReply* reply = m_network_access_mgr->get(request);

  QProgressDialog progress(tr("Downloading %1...").arg(m_download_url), tr("Cancel"), 0, 1);
  progress.setAutoClose(false);

  connect(reply, &QNetworkReply::downloadProgress, [&progress](quint64 received, quint64 total) {
    progress.setRange(0, static_cast<int>(total));
    progress.setValue(static_cast<int>(received));
  });

  connect(m_network_access_mgr, &QNetworkAccessManager::finished, [this, &progress](QNetworkReply* reply) {
    m_network_access_mgr->disconnect();

    if (reply->error() != QNetworkReply::NoError)
    {
      QMessageBox::critical(this, tr("Download failed"), reply->errorString());
      progress.done(-1);
      return;
    }

    const QString update_zip_path = m_host_interface->getProgramDirectoryRelativePath(QStringLiteral("update.zip"));
    const QString updater_path = m_host_interface->getProgramDirectoryRelativePath(QStringLiteral("updater.exe"));
    const QString update_directory = QCoreApplication::applicationDirPath();
    Q_ASSERT(!update_zip_path.isEmpty() && !updater_path.isEmpty() && !update_directory.isEmpty());
    if ((QFile::exists(update_zip_path) && !QFile::remove(update_zip_path)) ||
        (QFile::exists(updater_path) && !QFile::remove(updater_path)))
    {
      QMessageBox::critical(this, tr("Error"), tr("Removing existing update zip/updater failed"));
      progress.done(-1);
      return;
    }

    {
      const QByteArray data = reply->readAll();
      QFile update_zip_file(update_zip_path);
      if (!update_zip_file.open(QIODevice::WriteOnly) || update_zip_file.write(data) != data.size())
      {
        QMessageBox::critical(this, tr("Error"), tr("Writing update zip '%1' failed").arg(update_zip_path));
        progress.done(-1);
        return;
      }
      update_zip_file.close();
    }

    if (!extractUpdater(update_zip_path, updater_path))
    {
      QMessageBox::critical(this, tr("Error"), tr("Extracting updater failed"));
      progress.done(-1);
      return;
    }

    if (!doUpdate(update_zip_path, updater_path, update_directory))
    {
      QMessageBox::critical(this, tr("Error"), tr("Launching updater failed"));
      progress.done(-1);
      return;
    }

    progress.done(1);
  });

  const int result = progress.exec();
  if (result == 0)
  {
    // cancelled
    reply->abort();
  }
  else if (result == 1)
  {
    // updater started
    done(0);
  }

  reply->deleteLater();
}

bool AutoUpdaterDialog::updateNeeded() const
{
  return true;
}

void AutoUpdaterDialog::skipThisUpdateClicked()
{
  // TODO: Save to config
  done(0);
}

void AutoUpdaterDialog::remindMeLaterClicked()
{
  done(0);
}

bool AutoUpdaterDialog::extractUpdater(const QString& zip_path, const QString& destination_path)
{
  unzFile zf = unzOpen(zip_path.toUtf8().constData());
  if (!zf)
  {
    Log_ErrorPrintf("Failed to open update zip");
    return false;
  }

  if (unzLocateFile(zf, "updater.exe", 0) != UNZ_OK || unzOpenCurrentFile(zf) != UNZ_OK)
  {
    Log_ErrorPrintf("Failed to locate updater.exe");
    unzClose(zf);
    return false;
  }

  QFile updater_exe(destination_path);
  if (!updater_exe.open(QIODevice::WriteOnly))
  {
    Log_ErrorPrintf("Failed to open updater.exe for writing");
    unzClose(zf);
    return false;
  }

  static constexpr size_t CHUNK_SIZE = 4096;
  char chunk[CHUNK_SIZE];
  for (;;)
  {
    int size = unzReadCurrentFile(zf, chunk, CHUNK_SIZE);
    if (size < 0)
    {
      Log_ErrorPrintf("Failed to decompress updater exe");
      unzClose(zf);
      updater_exe.close();
      updater_exe.remove();
      return false;
    }
    else if (size == 0)
    {
      break;
    }

    if (updater_exe.write(chunk, size) != size)
    {
      Log_ErrorPrintf("Failed to write updater exe");
      unzClose(zf);
      updater_exe.close();
      updater_exe.remove();
      return false;
    }
  }

  unzClose(zf);
  updater_exe.close();
  return true;
}

bool AutoUpdaterDialog::doUpdate(const QString& zip_path, const QString& updater_path, const QString& destination_path)
{
  const QString program_path = QCoreApplication::applicationFilePath();
  if (program_path.isEmpty())
  {
    Log_ErrorPrintf("Failed to get current application path");
    return false;
  }

  QStringList arguments;
  arguments << QStringLiteral("%1").arg(QCoreApplication::applicationPid());
  arguments << destination_path;
  arguments << zip_path;
  arguments << program_path;

  // this will leak, but not sure how else to handle it...
  QProcess* updater_process = new QProcess();
  updater_process->setProgram(updater_path);
  updater_process->setArguments(arguments);
  updater_process->start(QIODevice::NotOpen);
  if (!updater_process->waitForStarted())
  {
    Log_ErrorPrintf("Failed to start updater");
    return false;
  }

  // quit the application ASAP so the updater can start
  m_host_interface->requestExit();
  return true;
}
