// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <boost/noncopyable.hpp>
#include <folly/ExceptionWrapper.h>
#include <folly/functional/Invoke.h>
#include <glog/logging.h>
#include "yarpl/Refcounted.h"
#include "yarpl/flowable/Subscription.h"
#include "yarpl/utils/credits.h"

namespace yarpl {
namespace flowable {

template <typename T>
class Subscriber : boost::noncopyable {
 public:
  virtual ~Subscriber() = default;
  virtual void onSubscribe(std::shared_ptr<Subscription>) = 0;
  virtual void onComplete() = 0;
  virtual void onError(folly::exception_wrapper) = 0;
  virtual void onNext(T) = 0;

  template <
      typename Next,
      typename =
          typename std::enable_if<folly::is_invocable<Next, T>::value>::type>
  static std::shared_ptr<Subscriber<T>> create(
      Next next,
      int64_t batch = credits::kNoFlowControl);

  template <
      typename Next,
      typename Error,
      typename = typename std::enable_if<
          folly::is_invocable<Next, T>::value &&
          folly::is_invocable<Error, folly::exception_wrapper>::value>::type>
  static std::shared_ptr<Subscriber<T>>
  create(Next next, Error error, int64_t batch = credits::kNoFlowControl);

  template <
      typename Next,
      typename Error,
      typename Complete,
      typename = typename std::enable_if<
          folly::is_invocable<Next, T>::value &&
          folly::is_invocable<Error, folly::exception_wrapper>::value &&
          folly::is_invocable<Complete>::value>::type>
  static std::shared_ptr<Subscriber<T>> create(
      Next next,
      Error error,
      Complete complete,
      int64_t batch = credits::kNoFlowControl);

  static std::shared_ptr<Subscriber<T>> create() {
    class NullSubscriber : public Subscriber<T> {
      void onSubscribe(std::shared_ptr<Subscription> s) override final {
        s->request(credits::kNoFlowControl);
      }

      void onNext(T) override final {}
      void onComplete() override {}
      void onError(folly::exception_wrapper) override {}
    };
    return std::make_shared<NullSubscriber>();
  }
};

#define KEEP_REF_TO_THIS()              \
  std::shared_ptr<BaseSubscriber> self; \
  if (keep_reference_to_this) {         \
    self = this->ref_from_this(this);   \
  }

// T : Type of Flowable that this Subscriber operates on
//
// keep_reference_to_this : BaseSubscriber will keep a live reference to
// itself on the stack while in a signaling or requesting method, in case
// the derived class causes all other references to itself to be dropped.
//
// Classes that ensure that at least one reference will stay live can
// use `keep_reference_to_this = false` as an optimization to
// prevent an atomic inc/dec pair
template <typename T, bool keep_reference_to_this = true>
class BaseSubscriber : public Subscriber<T>, public yarpl::enable_get_ref {
 public:
  // Note: If any of the following methods is overridden in a subclass, the new
  // methods SHOULD ensure that these are invoked as well.
  void onSubscribe(std::shared_ptr<Subscription> subscription) final override {
    CHECK(subscription);
    CHECK(!yarpl::atomic_load(&subscription_));

#ifndef NDEBUG
    DCHECK(!gotOnSubscribe_.exchange(true))
        << "Already subscribed to BaseSubscriber";
#endif

    yarpl::atomic_store(&subscription_, std::move(subscription));
    KEEP_REF_TO_THIS();
    onSubscribeImpl();
  }

  // No further calls to the subscription after this method is invoked.
  void onComplete() final override {
#ifndef NDEBUG
    DCHECK(gotOnSubscribe_.load()) << "Not subscribed to BaseSubscriber";
    DCHECK(!gotTerminating_.exchange(true))
        << "Already got terminating signal method";
#endif

    std::shared_ptr<Subscription> null;
    if (auto sub = yarpl::atomic_exchange(&subscription_, null)) {
      KEEP_REF_TO_THIS();
      onCompleteImpl();
      onTerminateImpl();
    }
  }

  // No further calls to the subscription after this method is invoked.
  void onError(folly::exception_wrapper e) final override {
#ifndef NDEBUG
    DCHECK(gotOnSubscribe_.load()) << "Not subscribed to BaseSubscriber";
    DCHECK(!gotTerminating_.exchange(true))
        << "Already got terminating signal method";
#endif

    std::shared_ptr<Subscription> null;
    if (auto sub = yarpl::atomic_exchange(&subscription_, null)) {
      KEEP_REF_TO_THIS();
      onErrorImpl(std::move(e));
      onTerminateImpl();
    }
  }

  void onNext(T t) final override {
#ifndef NDEBUG
    DCHECK(gotOnSubscribe_.load()) << "Not subscibed to BaseSubscriber";
    if (gotTerminating_.load()) {
      VLOG(2) << "BaseSubscriber already got terminating signal method";
    }
#endif

    if (auto sub = yarpl::atomic_load(&subscription_)) {
      KEEP_REF_TO_THIS();
      onNextImpl(std::move(t));
    }
  }

  void cancel() {
    std::shared_ptr<Subscription> null;
    if (auto sub = yarpl::atomic_exchange(&subscription_, null)) {
      KEEP_REF_TO_THIS();
      sub->cancel();
      onTerminateImpl();
    }
#ifndef NDEBUG
    else {
      VLOG(2) << "cancel() on BaseSubscriber with no subscription_";
    }
#endif
  }

  void request(int64_t n) {
    if (auto sub = yarpl::atomic_load(&subscription_)) {
      KEEP_REF_TO_THIS();
      sub->request(n);
    }
#ifndef NDEBUG
    else {
      VLOG(2) << "request() on BaseSubscriber with no subscription_";
    }
#endif
  }

 protected:
  virtual void onSubscribeImpl() = 0;
  virtual void onCompleteImpl() = 0;
  virtual void onNextImpl(T) = 0;
  virtual void onErrorImpl(folly::exception_wrapper) = 0;

  virtual void onTerminateImpl() {}

 private:
  // keeps a reference alive to the subscription
  AtomicReference<Subscription> subscription_;

#ifndef NDEBUG
  std::atomic<bool> gotOnSubscribe_{false};
  std::atomic<bool> gotTerminating_{false};
#endif
};

namespace details {
template <typename T, typename Next>
class Base : public BaseSubscriber<T> {
 public:
  Base(Next next, int64_t batch)
      : next_(std::move(next)), batch_(batch), pending_(0) {}

  void onSubscribeImpl() override final {
    pending_ += batch_;
    this->request(batch_);
  }

  void onNextImpl(T value) override final {
    try {
      next_(std::move(value));
    } catch (const std::exception& exn) {
      this->cancel();
      auto ew = folly::exception_wrapper{std::current_exception(), exn};
      LOG(ERROR) << "'next' method should not throw: " << ew.what();
      onErrorImpl(ew);
      return;
    }

    if (--pending_ < batch_ / 2) {
      const auto delta = batch_ - pending_;
      pending_ += delta;
      this->request(delta);
    }
  }

  void onCompleteImpl() override {}
  void onErrorImpl(folly::exception_wrapper) override {}

 private:
  Next next_;
  const int64_t batch_;
  int64_t pending_;
};

template <typename T, typename Next, typename Error>
class WithError : public Base<T, Next> {
 public:
  WithError(Next next, Error error, int64_t batch)
      : Base<T, Next>(std::move(next), batch), error_(std::move(error)) {}

  void onErrorImpl(folly::exception_wrapper error) override final {
    try {
      error_(std::move(error));
    } catch (const std::exception& exn) {
      auto ew = folly::exception_wrapper{std::current_exception(), exn};
      LOG(ERROR) << "'error' method should not throw: " << ew.what();
#ifndef NDEBUG
      throw ew; // Throw the wrapped exception
#endif
    }
  }

 private:
  Error error_;
};

template <typename T, typename Next, typename Error, typename Complete>
class WithErrorAndComplete : public WithError<T, Next, Error> {
 public:
  WithErrorAndComplete(Next next, Error error, Complete complete, int64_t batch)
      : WithError<T, Next, Error>(std::move(next), std::move(error), batch),
        complete_(std::move(complete)) {}

  void onCompleteImpl() override final {
    try {
      complete_();
    } catch (const std::exception& exn) {
      auto ew = folly::exception_wrapper{std::current_exception(), exn};
      LOG(ERROR) << "'complete' method should not throw: " << ew.what();
#ifndef NDEBUG
      throw ew; // Throw the wrapped exception
#endif
    }
  }

 private:
  Complete complete_;
};
} // namespace details

template <typename T>
template <typename Next, typename>
std::shared_ptr<Subscriber<T>> Subscriber<T>::create(Next next, int64_t batch) {
  return std::make_shared<details::Base<T, Next>>(std::move(next), batch);
}

template <typename T>
template <typename Next, typename Error, typename>
std::shared_ptr<Subscriber<T>>
Subscriber<T>::create(Next next, Error error, int64_t batch) {
  return std::make_shared<details::WithError<T, Next, Error>>(
      std::move(next), std::move(error), batch);
}

template <typename T>
template <typename Next, typename Error, typename Complete, typename>
std::shared_ptr<Subscriber<T>> Subscriber<T>::create(
    Next next,
    Error error,
    Complete complete,
    int64_t batch) {
  return std::make_shared<
      details::WithErrorAndComplete<T, Next, Error, Complete>>(
      std::move(next), std::move(error), std::move(complete), batch);
}

} // namespace flowable
} // namespace yarpl
