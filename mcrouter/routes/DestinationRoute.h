/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <folly/experimental/fibers/FiberManager.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/ScopeGuard.h>

#include "mcrouter/config-impl.h"
#include "mcrouter/config.h"
#include "mcrouter/LeaseTokenMap.h"
#include "mcrouter/lib/fbi/cpp/util.h"
#include "mcrouter/lib/McOperation.h"
#include "mcrouter/lib/McReply.h"
#include "mcrouter/lib/McRequest.h"
#include "mcrouter/lib/RouteHandleTraverser.h"
#include "mcrouter/McrouterFiberContext.h"
#include "mcrouter/McrouterInstance.h"
#include "mcrouter/proxy.h"
#include "mcrouter/ProxyDestination.h"
#include "mcrouter/ProxyRequestContext.h"
#include "mcrouter/routes/McrouterRouteHandle.h"

namespace facebook { namespace memcache { namespace mcrouter {

/**
 * Routes a request to a single ProxyClient.
 * This is the lowest level in Mcrouter's RouteHandle tree.
 */
class DestinationRoute {
 public:
  std::string routeName() const;

  /**
   * Spool failed delete request to disk.
   *
   * @return  true on sucess, false otherwise
   */
  bool spool(const McRequest& req) const;

  std::string keyWithFailoverTag(folly::StringPiece fullKey) const;

  /**
   * @param destination The destination where the request is to be sent
   */
  DestinationRoute(std::shared_ptr<ProxyDestination> destination,
                   std::string poolName,
                   size_t indexInPool,
                   std::chrono::milliseconds timeout,
                   bool keepRoutingPrefix) :
      destination_(std::move(destination)),
      poolName_(std::move(poolName)),
      indexInPool_(indexInPool),
      timeout_(timeout),
      keepRoutingPrefix_(keepRoutingPrefix) {
  }

  template <class Operation, class Request>
  void traverse(const Request& req, Operation,
                const RouteHandleTraverser<McrouterRouteHandleIf>& t) const {
    auto& ctx = fiber_local::getSharedCtx();
    if (ctx) {
      ctx->recordDestination(poolName_, indexInPool_,
                             *destination_->accessPoint());
    }
  }

  McReply route(const McRequest& req, McOperation<mc_op_lease_get> op) const {
    auto reply = routeWithDestination(req, op);
    if (reply.leaseToken() > 1 &&
        (fiber_local::getRequestClass().is(RequestClass::kFailover) ||
         LeaseTokenMap::conflicts(reply.leaseToken()))) {
      auto& ctx = fiber_local::getSharedCtx();
      if (auto leaseTokenMap = ctx->proxy().router().leaseTokenMap()) {
        auto specialToken = leaseTokenMap->insert(
            {reply.leaseToken(), poolName_, indexInPool_});
        reply.setLeaseToken(specialToken);
      }
    }
    return reply;
  }

  template <class Operation>
  McReply route(const McRequest& req, Operation,
                typename DeleteLike<Operation>::Type = 0) const {
    auto reply = routeWithDestination(req, Operation());
    if (reply.isFailoverError() && spool(req)) {
      reply = McReply(DefaultReply, Operation());
      reply.setDestination(destination_->accessPoint());
    }
    return reply;
  }

  McReply route(const McRequest& req, McOperation<mc_op_touch>) const {
    auto reply = routeWithDestination(req, McOperation<mc_op_touch>());
    if (reply.isFailoverError()) {
      reply = McReply(DefaultReply, McOperation<mc_op_touch>());
    }
    return reply;
  }

  template <class Operation>
  McReply route(const McRequest& req, Operation,
                OtherThanT(Operation, DeleteLike<>) = 0) const {
    return routeWithDestination(req, Operation());
  }

 private:
  const std::shared_ptr<ProxyDestination> destination_;
  const std::string poolName_;
  const size_t indexInPool_;
  const std::chrono::milliseconds timeout_;
  const bool keepRoutingPrefix_;
  size_t pendingShadowReqs_{0};

  template <int Op>
  McReply routeWithDestination(const McRequest& req, McOperation<Op>) const {
    auto reply = checkAndRoute(req, McOperation<Op>());
    reply.setDestination(destination_->accessPoint());
    return reply;
  }

  template <int Op>
  McReply checkAndRoute(const McRequest& req, McOperation<Op>) const {
    auto& ctx = fiber_local::getSharedCtx();
    if (!destination_->may_send()) {
      return constructAndLog(req, McOperation<Op>(), *ctx, TkoReply);
    }

    if (ctx->recording()) {
      ctx->recordDestination(poolName_, indexInPool_,
                             *destination_->accessPoint());
      return constructAndLog(
          req, McOperation<Op>(), *ctx, DefaultReply, McOperation<Op>());
    }

    auto proxy = &ctx->proxy();
    auto requestClass = fiber_local::getRequestClass();
    if (requestClass.is(RequestClass::kShadow)) {
      if (proxy->router().opts().target_max_shadow_requests > 0 &&
          pendingShadowReqs_ >=
          proxy->router().opts().target_max_shadow_requests) {
        return constructAndLog(req, McOperation<Op>(), *ctx, ErrorReply);
      }
      auto& mutableCounter = const_cast<size_t&>(pendingShadowReqs_);
      ++mutableCounter;
    }

    SCOPE_EXIT {
      if (requestClass.is(RequestClass::kShadow)) {
        auto& mutableCounter = const_cast<size_t&>(pendingShadowReqs_);
        --mutableCounter;
      }
    };

    return doRoute(req, McOperation<Op>(), *ctx);
  }

  template <int Op, class... Args>
  McReply constructAndLog(const McRequest& req,
                          McOperation<Op>,
                          ProxyRequestContext& ctx,
                          Args&&... args) const {
    auto now = nowUs();
    auto reply = McReply(std::forward<Args>(args)...);
    ctx.onReplyReceived(poolName_,
                        *destination_->accessPoint(),
                        req,
                        reply,
                        now,
                        now,
                        McOperation<Op>());
    return reply;
  }

  template <int Op>
  McReply doRoute(const McRequest& req,
                  McOperation<Op>,
                  ProxyRequestContext& ctx) const {
    DestinationRequestCtx dctx(nowUs());
    folly::Optional<McRequest> newReq;
    if (!keepRoutingPrefix_ && !req.routingPrefix().empty()) {
      newReq.emplace(req.clone());
      newReq->stripRoutingPrefix();
    }

    if (fiber_local::getFailoverTag()) {
      if (!newReq) {
        newReq.emplace(req.clone());
      }
      auto newKey = keyWithFailoverTag(newReq->fullKey());
      /* It's always safe to not append a failover tag */
      if (newKey.size() <= MC_KEY_MAX_LEN) {
        newReq->setKey(std::move(newKey));
      }
    }

    const McRequest& reqToSend = newReq ? *newReq : req;
    auto reply =
        destination_->send(reqToSend, McOperation<Op>(), dctx, timeout_);
    ctx.onReplyReceived(poolName_,
                        *destination_->accessPoint(),
                        reqToSend,
                        reply,
                        dctx.startTime,
                        dctx.endTime,
                        McOperation<Op>());
    return reply;
  }
};

}}}  // facebook::memcache::mcrouter
