#pragma once
#include "ui_autoupdaterdialog.h"
#include <QtWidgets/QDialog>

class QNetworkAccessManager;
class QNetworkReply;

class QtHostInterface;

class AutoUpdaterDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit AutoUpdaterDialog(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~AutoUpdaterDialog();

Q_SIGNALS:
  void updateCheckCompleted();

public Q_SLOTS:
  void queueUpdateCheck();
  void queueGetLatestRelease();

private Q_SLOTS:
  void getLatestTagComplete(QNetworkReply* reply);
  void getLatestReleaseComplete(QNetworkReply* reply);

  void downloadUpdateClicked();
  void skipThisUpdateClicked();
  void remindMeLaterClicked();

private:
  bool updateNeeded() const;
  bool extractUpdater(const QString& zip_path, const QString& destination_path);
  bool doUpdate(const QString& zip_path, const QString& updater_path, const QString& destination_path);

  Ui::AutoUpdaterDialog m_ui;

  QtHostInterface* m_host_interface;
  QNetworkAccessManager* m_network_access_mgr = nullptr;
  QString m_latest_sha;
  QString m_download_url;
};
