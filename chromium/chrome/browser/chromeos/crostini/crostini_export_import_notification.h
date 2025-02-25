// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_CROSTINI_CROSTINI_EXPORT_IMPORT_NOTIFICATION_H_
#define CHROME_BROWSER_CHROMEOS_CROSTINI_CROSTINI_EXPORT_IMPORT_NOTIFICATION_H_

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "chrome/browser/chromeos/crostini/crostini_util.h"
#include "ui/message_center/public/cpp/notification_delegate.h"

class Profile;

namespace message_center {
class Notification;
}

namespace crostini {

enum class ExportImportType;

// Notification for Crostini export and import.
class CrostiniExportImportNotification
    : public message_center::NotificationObserver {
 public:
  enum class Status { RUNNING, DONE, FAILED };

  // Used to construct CrostiniExportImportNotification to ensure it controls
  // its lifetime.
  static CrostiniExportImportNotification* Create(
      Profile* profile,
      ExportImportType type,
      const std::string& notification_id,
      const base::FilePath& path) {
    return new CrostiniExportImportNotification(profile, type, notification_id,
                                                path);
  }

  virtual ~CrostiniExportImportNotification();

  void SetStatusRunning(int progress_percent);
  void SetStatusDone();
  void SetStatusFailed();
  void SetStatusFailedArchitectureMismatch(
      const std::string& architecture_container,
      const std::string& architecture_device);
  void SetStatusFailedInsufficientSpace(uint64_t additional_required_space);
  void SetStatusFailedConcurrentOperation(
      ExportImportType in_progress_operation_type);

  Status status() const { return status_; }
  ExportImportType type() const { return type_; }
  const base::FilePath& path() const { return path_; }
  // Getters for testing.
  message_center::Notification* get_notification() {
    return notification_.get();
  }

  // message_center::NotificationObserver:
  void Close(bool by_user) override;
  void Click(const base::Optional<int>& button_index,
             const base::Optional<base::string16>& reply) override;

 private:
  CrostiniExportImportNotification(Profile* profile,
                                   ExportImportType type,
                                   const std::string& notification_id,
                                   const base::FilePath& path);

  void SetStatusFailed(const base::string16& message);

  Profile* profile_;
  ExportImportType type_;
  base::FilePath path_;
  Status status_ = Status::RUNNING;
  // Time when the operation started.  Used for estimating time remaining.
  base::TimeTicks started_ = base::TimeTicks::Now();
  std::unique_ptr<message_center::Notification> notification_;
  bool closed_ = false;
  base::WeakPtrFactory<CrostiniExportImportNotification> weak_ptr_factory_;
  DISALLOW_COPY_AND_ASSIGN(CrostiniExportImportNotification);
};

}  // namespace crostini

#endif  // CHROME_BROWSER_CHROMEOS_CROSTINI_CROSTINI_EXPORT_IMPORT_NOTIFICATION_H_
