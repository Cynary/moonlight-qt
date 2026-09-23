#include "nativesteam.h"
#include <Limelight.h>
#include <NativeController.h>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QProcess>
#include <cstring>
namespace {
QByteArray message(unsigned char op) {
  QByteArray m(80, 0);
  m[0] = 1;
  m[1] = op;
  return m;
}
bool send(const QByteArray &m) {
  return LiSendNativeControllerMessage(
             reinterpret_cast<const uint8_t *>(m.constData()), m.size()) == 0;
}
} // namespace
QString NativeSteam::findDevice(bool enabled) {
#ifdef Q_OS_LINUX
    const auto explicitDevice = qEnvironmentVariable("MOONMACHINE_NATIVE_CONTROLLER_DEVICE");
    if (!explicitDevice.isEmpty()) return explicitDevice;
    if (!enabled) return {};
    for (const auto& name : QDir("/sys/class/hidraw").entryList({"hidraw*"}, QDir::Dirs)) {
        const auto device = QFileInfo("/sys/class/hidraw/" + name + "/device").canonicalFilePath();
        QFile identity(device + "/uevent"), interfaceNumber(device + "/../bInterfaceNumber");
        if (identity.open(QIODevice::ReadOnly) && interfaceNumber.open(QIODevice::ReadOnly) &&
            identity.readAll().contains("HID_ID=0003:000028DE:00001304") &&
            interfaceNumber.readAll().trimmed() == "02") return "/dev/" + name;
    }
#else
    Q_UNUSED(enabled);
#endif
    return {};
}
NativeSteam::~NativeSteam() { stop(); }
void NativeSteam::stop() {
  stopping = true;
  if (worker.joinable())
    worker.join();
  accepted = false;
}
void NativeSteam::receive(const unsigned char *data, unsigned size) {
  if (!LiNativeValidate(data, size, 1))
    return;
  if (data[1] == LI_NATIVE_ACCEPT) {
    accepted = true;
    qInfo() << "Native Steam Controller accepted by host";
    return;
  }
  if (data[1] == LI_NATIVE_ERROR) {
    qWarning() << "Host rejected native Steam Controller";
    stopping = true;
    return;
  }
  std::lock_guard<std::mutex> lock(mutex);
  if (requests.size() < 32)
    requests.emplace_back(reinterpret_cast<const char *>(data), size);
  else {
    auto m = message(LI_NATIVE_REPLY);
    m[2] = 1;
    std::memcpy(m.data() + 8, data + 8, 8);
    send(m);
  }
}
void NativeSteam::start(const QString &device) {
  stop();
  stopping = false;
  accepted = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    requests.clear();
  }
  worker = std::thread([this, device] {
    QProcess helper;
    QString executable =
        qEnvironmentVariable("MOONMACHINE_NATIVE_CONTROLLER_HELPER",
                             "/usr/libexec/moonmachine-native-controller");
    helper.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    helper.start(executable, {device});
    if (!helper.waitForStarted(3000)) {
      qWarning() << "Native controller helper failed:" << helper.errorString();
      return;
    }
    unsigned reports = 0, replies = 0;
    QByteArray buffer;
    bool attached = false;
    QElapsedTimer timer;
    timer.start();
    while (!stopping && helper.state() != QProcess::NotRunning) {
      if (!attached && timer.elapsed() > 3000)
        break;
      if (attached && !accepted && timer.elapsed() > 5000)
        break;
      std::deque<QByteArray> pending;
      {
        std::lock_guard<std::mutex> lock(mutex);
        pending.swap(requests);
      }
      for (const auto &request : pending) {
        auto p = reinterpret_cast<const uint8_t *>(request.constData());
        helper.write(
            "Q " + QByteArray::number(LiNativeRequestId(p)) + " " +
            QByteArray::number(p[3]) + " " +
            QByteArray(reinterpret_cast<const char *>(p + 16), p[4]).toHex() +
            "\n");
      }
      helper.waitForReadyRead(5);
      buffer += helper.readAllStandardOutput();
      if (buffer.size() > 131072)
        break;
      int newline;
      while ((newline = buffer.indexOf('\n')) >= 0) {
        auto line = buffer.left(newline);
        buffer.remove(0, newline + 1);
        if (line == "READY" && !attached) {
          attached = send(message(LI_NATIVE_ATTACH));
          if (!attached)
            stopping = true;
          continue;
        }
        auto parts = line.split(' ');
        if (parts.size() == 2 && parts[0] == "I" && accepted) {
          auto report = QByteArray::fromHex(parts[1]);
          if (report.size() > 64)
            continue;
          auto m = message(LI_NATIVE_INPUT);
          m[4] = report.size();
          std::memcpy(m.data() + 16, report.constData(), report.size());
          if (!send(m))
            stopping = true;
          else
            reports++;
        } else if (parts.size() == 4 && parts[0] == "R") {
          bool ok = false;
          auto id = parts[1].toULongLong(&ok);
          if (!ok || !id)
            continue;
          auto m = message(LI_NATIVE_REPLY);
          LiNativeSetRequestId(reinterpret_cast<uint8_t *>(m.data()), id);
          bool success = parts[2] == "0";
          m[2] = success ? 0 : 1;
          auto payload =
              parts[3] == "-" ? QByteArray() : QByteArray::fromHex(parts[3]);
          if (payload.size() > 64)
            continue;
          m[4] = payload.size();
          std::memcpy(m.data() + 16, payload.constData(), payload.size());
          if (!send(m))
            stopping = true;
          else
            replies++;
        }
      }
    }
    if (attached)
      send(message(LI_NATIVE_DETACH));
    accepted = false;
    qInfo() << "Native Steam Controller stopped; reports" << reports
            << "feature replies" << replies;
    helper.write("X\n");
    helper.closeWriteChannel();
    if (!helper.waitForFinished(1000)) {
      helper.kill();
      helper.waitForFinished(1000);
    }
  });
}
