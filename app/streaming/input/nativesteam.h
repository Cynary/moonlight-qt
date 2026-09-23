#pragma once
#include <QByteArray>
#include <QString>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
class NativeSteam {
public:
  static QString findDevice(bool enabled);
  NativeSteam() = default;
  ~NativeSteam();
  void start(const QString &device);
  void stop();
  void receive(const unsigned char *data, unsigned size);
  bool active() const { return accepted.load(); }

private:
  std::atomic_bool stopping{false}, accepted{false};
  std::thread worker;
  std::mutex mutex;
  std::deque<QByteArray> requests;
};
