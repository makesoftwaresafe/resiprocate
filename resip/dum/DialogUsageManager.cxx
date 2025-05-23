
#if defined(HAVE_CONFIG_H)
  #include "config.h"
#endif

#include "resip/stack/SecurityAttributes.hxx"
#include "resip/stack/ShutdownMessage.hxx"
#include "resip/stack/SipFrag.hxx"
#include "resip/stack/SipMessage.hxx"
#include "resip/stack/SipStack.hxx"
#include "resip/stack/Helper.hxx"
#include "resip/stack/TransactionUserMessage.hxx"
#include "resip/stack/ConnectionTerminated.hxx"
#include "resip/stack/KeepAlivePong.hxx"
#include "resip/dum/AppDialog.hxx"
#include "resip/dum/AppDialogSet.hxx"
#include "resip/dum/AppDialogSetFactory.hxx"
#include "resip/dum/BaseUsage.hxx"
#include "resip/dum/ClientAuthManager.hxx"
#include "resip/dum/ClientInviteSession.hxx"
#include "resip/dum/ClientOutOfDialogReq.hxx"
#include "resip/dum/ClientPublication.hxx"
#include "resip/dum/ClientRegistration.hxx"
#include "resip/dum/ClientSubscription.hxx"
#include "resip/dum/DefaultServerReferHandler.hxx"
#include "resip/dum/DestroyUsage.hxx"
#include "resip/dum/Dialog.hxx"
#include "resip/dum/DialogEventStateManager.hxx"
#include "resip/dum/DialogEventHandler.hxx"
#include "resip/dum/DialogUsageManager.hxx"
#include "resip/dum/ClientPagerMessage.hxx"
#include "resip/dum/DumException.hxx"
#include "resip/dum/DumShutdownHandler.hxx"
#include "resip/dum/DumFeatureMessage.hxx"
#include "resip/dum/ExternalMessageBase.hxx"
#include "resip/dum/ExternalMessageHandler.hxx"
#include "resip/dum/InviteSessionCreator.hxx"
#include "resip/dum/InviteSessionHandler.hxx"
#include "resip/dum/KeepAliveManager.hxx"
#include "resip/dum/KeepAliveTimeout.hxx"
#include "resip/dum/MasterProfile.hxx"
#include "resip/dum/OutOfDialogReqCreator.hxx"
#include "resip/dum/PagerMessageCreator.hxx"
#include "resip/dum/PublicationCreator.hxx"
#include "resip/dum/RedirectManager.hxx"
#include "resip/dum/RegistrationCreator.hxx"
#include "resip/dum/RemoteCertStore.hxx"
#include "resip/dum/RequestValidationHandler.hxx"
#include "resip/dum/ServerAuthManager.hxx"
#include "resip/dum/ServerInviteSession.hxx"
#include "resip/dum/ServerPublication.hxx"
#include "resip/dum/ServerSubscription.hxx"
#include "resip/dum/SubscriptionCreator.hxx"
#include "resip/dum/SubscriptionHandler.hxx"
#include "resip/dum/UserAuthInfo.hxx"
#include "resip/dum/DumFeature.hxx"
#include "resip/dum/IdentityHandler.hxx"
#include "resip/dum/DumDecrypted.hxx"
#include "resip/dum/CertMessage.hxx"
#include "resip/dum/OutgoingEvent.hxx"
#include "resip/dum/DumHelper.hxx"
#include "resip/dum/MergedRequestRemovalCommand.hxx"
#include "resip/dum/InMemorySyncPubDb.hxx"
#include "rutil/ResipAssert.h"
#include "rutil/Inserter.hxx"
#include "rutil/Logger.hxx"
#include "rutil/Random.hxx"
#include "rutil/Mutex.hxx"
#include "rutil/Timer.hxx"
#include "rutil/WinLeakCheck.hxx"

#ifdef USE_SSL
#include "resip/stack/ssl/Security.hxx"
#include "resip/dum/ssl/EncryptionManager.hxx"
#endif

#include <utility>

#define RESIPROCATE_SUBSYSTEM Subsystem::DUM

using namespace resip;
using namespace std;

#ifdef RESIP_DUM_THREAD_DEBUG
#define threadCheck()                                                   \
   do                                                                   \
   {                                                                    \
      if(mThreadDebugKey)                                               \
      {                                                                 \
         resip_assert(ThreadIf::tlsGetValue(mThreadDebugKey));                \
      }                                                                 \
   } while (false)
#else
#define threadCheck() void()
#endif


DialogUsageManager::DialogUsageManager(SipStack& stack, bool createDefaultFeatures) :
   TransactionUser(TransactionUser::DoNotRegisterForTransactionTermination, 
                   TransactionUser::RegisterForConnectionTermination,
                   TransactionUser::RegisterForKeepAlivePongs),
   mRedirectManager(new RedirectManager()),
   mInviteSessionHandler(0),
   mClientRegistrationHandler(0),
   mServerRegistrationHandler(0),
   mRedirectHandler(0),
   mDialogSetHandler(0),
   mRequestValidationHandler(0),
   mRegistrationPersistenceManager(0),
   mPublicationPersistenceManager(0),
   mIsDefaultServerReferHandler(true),
   mClientPagerMessageHandler(0),
   mServerPagerMessageHandler(0),
   mDialogEventStateManager(0),
   mAppDialogSetFactory(new AppDialogSetFactory()),
   mStack(stack),
   mDumShutdownHandler(0),
   mShutdownState(Running),
   mThreadDebugKey(0),
   mHiddenThreadDebugKey(0)
{
   //TODO -- create default features
   mStack.registerTransactionUser(*this);
   addServerSubscriptionHandler("refer", new DefaultServerReferHandler());

   mFifo.setDescription("DialogUsageManager::mFifo");

   mIncomingTarget = new IncomingTarget(*this);
   mOutgoingTarget = new OutgoingTarget(*this);

   if (createDefaultFeatures)
   {
      auto identity = std::make_shared<IdentityHandler>(*this, *mIncomingTarget);

#if defined (USE_SSL)
      auto encryptionIncoming = std::make_shared<EncryptionManager>(*this, *mIncomingTarget);
      auto encryptionOutgoing = std::make_shared<EncryptionManager>(*this, *mOutgoingTarget);
#endif

      // default incoming features.
      addIncomingFeature(identity);
#if defined (USE_SSL)
      addIncomingFeature(encryptionIncoming);
#endif

      // default outgoing features.
#if defined (USE_SSL)
      addOutgoingFeature(encryptionOutgoing);
#endif
   }
}

DialogUsageManager::~DialogUsageManager()
{
   mShutdownState = Destroying;
   //InfoLog ( << "~DialogUsageManager" );

   if(!mDialogSetMap.empty())
   {
      DebugLog(<< "DialogUsageManager::mDialogSetMap has " << mDialogSetMap.size() << " DialogSets");
      DialogSetMap::const_iterator ds = mDialogSetMap.begin();
      for(; ds != mDialogSetMap.end(); ++ds)
      {
         DebugLog(<< "DialgSetId:" << ds->first);
         DialogSet::DialogMap::const_iterator   d = ds->second->mDialogs.begin();
         for(; d != ds->second->mDialogs.end(); ++d)
         {
            //const Dialog* p = &(d->second);
            DebugLog(<<"DialogId:" << d->first << ", " << *d->second);
         }
      }
   }

   while(!mDialogSetMap.empty())
   {
      DialogSet*  ds = mDialogSetMap.begin()->second;
      delete ds;  // Deleting a dialog set removes itself from the map
   }

   if(mIsDefaultServerReferHandler)
   {
       delete mServerSubscriptionHandlers["refer"];
   }

   delete mIncomingTarget;
   delete mOutgoingTarget;

   // Delete Server Publications
   while (!mServerPublications.empty())
   {
       delete mServerPublications.begin()->second;  // Deleting a ServerPublication removes itself from the map
   }

   // Remove any lingering incoming feature chain memory
   for(FeatureChainMap::iterator it = mIncomingFeatureChainMap.begin(); it != mIncomingFeatureChainMap.end(); it++)
   {
      delete it->second;
   }

   //InfoLog ( << "~DialogUsageManager done" );
}

const Data& 
DialogUsageManager::name() const
{
   static Data n("DialogUsageManager");
   return n;
}

void
DialogUsageManager::addTransport( TransportType protocol,
                                  int port,
                                  IpVersion version,
                                  const Data& ipInterface,
                                  const Data& sipDomainname, // only used
                                  const Data& privateKeyPassPhrase,
                                  SecurityTypes::SSLType sslType,
                                  unsigned transportFlags)
{
   mStack.addTransport(protocol, port, version, StunDisabled, ipInterface,
                       sipDomainname, privateKeyPassPhrase, sslType,
                       transportFlags);
}

SipStack& 
DialogUsageManager::getSipStack()
{
   return mStack;
}

const SipStack& 
DialogUsageManager::getSipStack() const
{
   return mStack;
}

Security*
DialogUsageManager::getSecurity()
{
   return mStack.getSecurity();
}

Data
DialogUsageManager::getHostAddress()
{
   return mStack.getHostAddress();
}

void
DialogUsageManager::onAllHandlesDestroyed()
{
   if (mDumShutdownHandler)
   {
      switch (mShutdownState)
      {
         case ShutdownRequested:
            InfoLog (<< "DialogUsageManager::onAllHandlesDestroyed: removing TU");
            //resip_assert(mHandleMap.empty());
            mShutdownState = RemovingTransactionUser;
            mStack.unregisterTransactionUser(*this);
            break;
         default:
            break;
      }
   }
}


void
DialogUsageManager::shutdown(DumShutdownHandler* h)
{
   InfoLog (<< "shutdown: dialogSets=" << mDialogSetMap.size());
   
   mDumShutdownHandler = h;
   mShutdownState = ShutdownRequested;
   mStack.requestTransactionUserShutdown(*this);
   shutdownWhenEmpty();
}

// void
// DialogUsageManager::shutdownIfNoUsages(DumShutdownHandler* h)
// {
//    InfoLog (<< "shutdown when no usages");
// 
//    mDumShutdownHandler = h;
//    mShutdownState = ShutdownRequested;
//    resip_assert(0);
// }

void
DialogUsageManager::forceShutdown(DumShutdownHandler* h)
{
   WarningLog (<< "force shutdown ");
   dumpHandles();
   
   mDumShutdownHandler = h;
   //HandleManager::shutdown();  // clear out usages
   mShutdownState = ShutdownRequested;
   DialogUsageManager::onAllHandlesDestroyed();
}

void DialogUsageManager::setAppDialogSetFactory(std::unique_ptr<AppDialogSetFactory> factory) noexcept
{
   mAppDialogSetFactory = std::move(factory);
}

std::shared_ptr<MasterProfile>&
DialogUsageManager::getMasterProfile()
{
   resip_assert(mMasterProfile.get());
   return mMasterProfile;
}

std::shared_ptr<UserProfile>&
DialogUsageManager::getMasterUserProfile()
{
   resip_assert(mMasterUserProfile.get());
   return mMasterUserProfile;
}

void DialogUsageManager::setMasterProfile(const std::shared_ptr<MasterProfile>& masterProfile)
{
   resip_assert(!mMasterProfile.get());
   mMasterProfile = masterProfile;
   mMasterUserProfile = masterProfile; // required so that we can return a reference to std::shared_ptr<UserProfile> in getMasterUserProfile
}

void DialogUsageManager::setKeepAliveManager(std::unique_ptr<KeepAliveManager> manager) noexcept
{
   mKeepAliveManager = std::move(manager);
   mKeepAliveManager->setDialogUsageManager(this);
}

void DialogUsageManager::setRedirectManager(std::unique_ptr<RedirectManager> manager) noexcept
{
   mRedirectManager = std::move(manager);
}

void DialogUsageManager::setRedirectHandler(RedirectHandler* handler) noexcept
{
   mRedirectHandler = handler;
}

RedirectHandler* DialogUsageManager::getRedirectHandler() const noexcept
{
   return mRedirectHandler;
}

void
DialogUsageManager::setClientAuthManager(std::unique_ptr<ClientAuthManager> manager) noexcept
{
   mClientAuthManager = std::move(manager);
}

void
DialogUsageManager::setServerAuthManager(std::shared_ptr<ServerAuthManager> manager)
{
   mIncomingFeatureList.emplace(std::begin(mIncomingFeatureList), manager);
}

void
DialogUsageManager::setClientRegistrationHandler(ClientRegistrationHandler* handler)
{
   resip_assert(!mClientRegistrationHandler);
   mClientRegistrationHandler = handler;
}

void
DialogUsageManager::setServerRegistrationHandler(ServerRegistrationHandler* handler)
{
   resip_assert(!mServerRegistrationHandler);
   mServerRegistrationHandler = handler;
}

void
DialogUsageManager::setDialogSetHandler(DialogSetHandler* handler) noexcept
{
   mDialogSetHandler = handler;
}

void
DialogUsageManager::setInviteSessionHandler(InviteSessionHandler* handler)
{
   resip_assert(!mInviteSessionHandler);
   mInviteSessionHandler = handler;
}

void
DialogUsageManager::setRequestValidationHandler(RequestValidationHandler* handler)
{
   resip_assert(!mRequestValidationHandler);
   mRequestValidationHandler = handler;
}

void
DialogUsageManager::setRegistrationPersistenceManager(RegistrationPersistenceManager* manager)
{
   resip_assert(!mRegistrationPersistenceManager);
   mRegistrationPersistenceManager = manager;
}

void
DialogUsageManager::setPublicationPersistenceManager(PublicationPersistenceManager* manager)
{
   resip_assert(!mPublicationPersistenceManager);
   mPublicationPersistenceManager = manager;
}

void
DialogUsageManager::addTimer(DumTimeout::Type type, unsigned long duration,
                             BaseUsageHandle target, unsigned int cseq, unsigned int rseq)
{
   DumTimeout t(type, duration, target, cseq, rseq);
   mStack.post(t, duration, this);
}

void
DialogUsageManager::addTimerMs(DumTimeout::Type type, unsigned long duration,
                               BaseUsageHandle target, unsigned int cseq, unsigned int rseq,
                               const Data &transactionId /*= Data::Empty*/)
{
   DumTimeout t(type, duration, target, cseq, rseq, transactionId);
   mStack.postMS(t, duration, this);
}

void
DialogUsageManager::addClientSubscriptionHandler(const Data& eventType, ClientSubscriptionHandler* handler)
{
   resip_assert(handler);
   resip_assert(mClientSubscriptionHandlers.find(eventType) == mClientSubscriptionHandlers.end());
   mClientSubscriptionHandlers[eventType] = handler;
}

void
DialogUsageManager::addServerSubscriptionHandler(const Data& eventType, ServerSubscriptionHandler* handler)
{
   resip_assert(handler);
   //default do-nothing server side refer handler can be replaced
   if (eventType == "refer")
   {
      auto it = mServerSubscriptionHandlers.find(eventType);
      if (it != mServerSubscriptionHandlers.end())
      {
         delete it->second;
         mIsDefaultServerReferHandler = false;
         //mServerSubscriptionHandlers.erase(eventType);
      }
   }

   mServerSubscriptionHandlers[eventType] = handler;
}

void
DialogUsageManager::addClientPublicationHandler(const Data& eventType, ClientPublicationHandler* handler)
{
   resip_assert(handler);
   resip_assert(mClientPublicationHandlers.find(eventType) == mClientPublicationHandlers.end());
   mClientPublicationHandlers[eventType] = handler;
}

void
DialogUsageManager::addServerPublicationHandler(const Data& eventType, ServerPublicationHandler* handler)
{
   resip_assert(handler);
   resip_assert(mServerPublicationHandlers.find(eventType) == mServerPublicationHandlers.end());
   mServerPublicationHandlers[eventType] = handler;
}

void
DialogUsageManager::addOutOfDialogHandler(MethodTypes type, OutOfDialogHandler* handler)
{
   resip_assert(handler);
   resip_assert(mOutOfDialogHandlers.find(type) == mOutOfDialogHandlers.end());
   mOutOfDialogHandlers[type] = handler;
}

void
DialogUsageManager::setClientPagerMessageHandler(ClientPagerMessageHandler* handler) noexcept
{
   mClientPagerMessageHandler = handler;
}

void
DialogUsageManager::setServerPagerMessageHandler(ServerPagerMessageHandler* handler) noexcept
{
   mServerPagerMessageHandler = handler;
}

void
DialogUsageManager::addExternalMessageHandler(ExternalMessageHandler* handler)
{
   std::vector<ExternalMessageHandler*>::iterator found = std::find(mExternalMessageHandlers.begin(), mExternalMessageHandlers.end(), handler);
   if (found == mExternalMessageHandlers.end())
   {
      mExternalMessageHandlers.push_back(handler);
   }
}

void 
DialogUsageManager::removeExternalMessageHandler(ExternalMessageHandler* handler)
{
   std::vector<ExternalMessageHandler*>::iterator found = std::find(mExternalMessageHandlers.begin(), mExternalMessageHandlers.end(), handler);
   if (found != mExternalMessageHandlers.end())
   {
      mExternalMessageHandlers.erase(found);
   }
}

void 
DialogUsageManager::clearExternalMessageHandler()
{
   std::vector<ExternalMessageHandler*> empty;
   empty.swap(mExternalMessageHandlers);
}


DialogSet*
DialogUsageManager::makeUacDialogSet(BaseCreator* creator, AppDialogSet* appDs)
{
   threadCheck();
   if (mDumShutdownHandler)
   {
      throw DumException("Cannot create new sessions when DUM is shutting down.", __FILE__, __LINE__);
   }

   if (appDs == 0)
   {
      appDs = new AppDialogSet(*this);
   }
   DialogSet* ds = new DialogSet(creator, *this);

   appDs->mDialogSet = ds;
   ds->mAppDialogSet = appDs;

   StackLog ( << "************* Adding DialogSet ***************: " << ds->getId());
   //StackLog ( << "Before: " << InserterP(mDialogSetMap) );
   mDialogSetMap[ds->getId()] = ds;
   StackLog ( << "DialogSetMap: " << InserterP(mDialogSetMap) );
   return ds;
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeNewSession(BaseCreator* creator, AppDialogSet* appDs)
{
   makeUacDialogSet(creator, appDs);
   return creator->getLastRequest();
}

void
DialogUsageManager::makeResponse(SipMessage& response,
                                 const SipMessage& request,
                                 int responseCode,
                                 const Data& reason) const
{
   resip_assert(request.isRequest());
   Helper::makeResponse(response, request, responseCode, reason);
}

void
DialogUsageManager::sendResponse(const SipMessage& response)
{
   resip_assert(response.isResponse());
   mStack.send(response, this);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeInviteSession(const NameAddr& target, const std::shared_ptr<UserProfile>& userProfile, const Contents* initialOffer, AppDialogSet* appDs)
{
   return makeInviteSession(target, userProfile, initialOffer, None, nullptr, appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeInviteSession(const NameAddr& target, const Contents* initialOffer, AppDialogSet* appDs)
{
   return makeInviteSession(target, getMasterUserProfile(), initialOffer, None, nullptr, appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeInviteSession(const NameAddr& target, 
                                      const std::shared_ptr<UserProfile>& userProfile,
                                      const Contents* initialOffer, 
                                      EncryptionLevel level, 
                                      const Contents* alternative, 
                                      AppDialogSet* appDs)
{
   auto inv = makeNewSession(new InviteSessionCreator(*this, target, userProfile, initialOffer, level, alternative), appDs);
   DumHelper::setOutgoingEncryptionLevel(*inv, level);
   return inv;
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeInviteSession(const NameAddr& target, 
                                      const Contents* initialOffer, 
                                      EncryptionLevel level, 
                                      const Contents* alternative,
                                      AppDialogSet* appDs)
{
   return makeInviteSession(target, getMasterUserProfile(), initialOffer, level, alternative, appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeInviteSession(const NameAddr& target,
    const DialogSetId& dialogSetId,
    const std::shared_ptr<UserProfile>& userProfile,
    const Contents* initialOffer,
    EncryptionLevel level,
    const Contents* alternative,
    AppDialogSet* appDs)
{
    resip_assert(mDialogSetMap.find(dialogSetId) == mDialogSetMap.end());
    BaseCreator* baseCreator(new InviteSessionCreator(*this, target, userProfile, initialOffer, level, alternative));
    baseCreator->getLastRequest()->header(h_CallID).value() = dialogSetId.getCallId();
    baseCreator->getLastRequest()->header(h_From).param(p_tag) = dialogSetId.getLocalTag();
    auto inv = makeNewSession(baseCreator, appDs);
    DumHelper::setOutgoingEncryptionLevel(*inv, level);
    return inv;
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeInviteSession(const NameAddr& target, 
                                      InviteSessionHandle sessionToReplace, 
                                      const std::shared_ptr<UserProfile>& userProfile,
                                      const Contents* initialOffer, 
                                      AppDialogSet* ads)
{
   auto inv = makeInviteSession(target, userProfile, initialOffer, ads);
   // add replaces header
   resip_assert(sessionToReplace.isValid());
   if(sessionToReplace.isValid())
   {
      CallId replaces;
      const DialogId& id = sessionToReplace->getDialogId();
      replaces.value() = id.getCallId();
      replaces.param(p_toTag) = id.getRemoteTag();
      replaces.param(p_fromTag) = id.getLocalTag();
      inv->header(h_Replaces) = replaces;
   }
   return inv;
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeInviteSession(const NameAddr& target, 
                                      InviteSessionHandle sessionToReplace, 
                                      const std::shared_ptr<UserProfile>& userProfile,
                                      const Contents* initialOffer, 
                                      EncryptionLevel level, 
                                      const Contents* alternative, 
                                      AppDialogSet* ads)
{
   auto inv = makeInviteSession(target, userProfile, initialOffer, level, alternative, ads);
   // add replaces header
   resip_assert(sessionToReplace.isValid());
   if(sessionToReplace.isValid())
   {
      CallId replaces;
      const DialogId& id = sessionToReplace->getDialogId();
      replaces.value() = id.getCallId();
      replaces.param(p_toTag) = id.getRemoteTag();
      replaces.param(p_fromTag) = id.getLocalTag();
      inv->header(h_Replaces) = replaces;
   }
   return inv;
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeInviteSession(const NameAddr& target, 
                                      InviteSessionHandle sessionToReplace, 
                                      const Contents* initialOffer, 
                                      EncryptionLevel level, 
                                      const Contents* alternative , 
                                      AppDialogSet* ads)
{
   auto inv = makeInviteSession(target, initialOffer, level, alternative, ads);
   // add replaces header
   resip_assert(sessionToReplace.isValid());
   if(sessionToReplace.isValid())
   {
      CallId replaces;
      const DialogId& id = sessionToReplace->getDialogId();
      replaces.value() = id.getCallId();
      replaces.param(p_toTag) = id.getRemoteTag();
      replaces.param(p_fromTag) = id.getLocalTag();
      inv->header(h_Replaces) = replaces;
   }
   return inv;
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeInviteSessionFromRefer(const SipMessage& refer,
                                               ServerSubscriptionHandle serverSub,
                                               const Contents* initialOffer,
                                               AppDialogSet* appDs)
{
   return makeInviteSessionFromRefer(refer, serverSub, initialOffer, None, 0, appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeInviteSessionFromRefer(const SipMessage& refer,
                                               const std::shared_ptr<UserProfile>& userProfile,
                                               const Contents* initialOffer,
                                               AppDialogSet* appDs)
{
   ServerSubscriptionHandle empty;
   return makeInviteSessionFromRefer(refer, userProfile, empty, initialOffer, None, 0, appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeInviteSessionFromRefer(const SipMessage& refer,
                                               ServerSubscriptionHandle serverSub,
                                               const Contents* initialOffer,
                                               EncryptionLevel level,
                                               const Contents* alternative,
                                               AppDialogSet* appDs)
{
   return makeInviteSessionFromRefer(refer, serverSub.isValid() ? serverSub->mDialog.mDialogSet.getUserProfile() : getMasterUserProfile(), serverSub, initialOffer, level, alternative, appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeInviteSessionFromRefer(const SipMessage& refer,
                                               const std::shared_ptr<UserProfile>& userProfile,
                                               ServerSubscriptionHandle serverSub,
                                               const Contents* initialOffer,
                                               EncryptionLevel level,
                                               const Contents* alternative,
                                               AppDialogSet* appDs)
{
   if (serverSub.isValid())
   {
      DebugLog(<< "implicit subscription");
      //generate and send 100
      SipFrag contents;
      contents.message().header(h_StatusLine).statusCode() = 100;
      contents.message().header(h_StatusLine).reason() = "Trying";
      //will be cloned...ServerSub may not have the most efficient API possible
      serverSub->setSubscriptionState(Active);
      auto notify = serverSub->update(&contents);
//   mInviteSessionHandler->onReadyToSend(InviteSessionHandle::NotValid(), notify);
      serverSub->send(notify);
   }

   //19.1.5
   NameAddr target = refer.header(h_ReferTo);
   target.uri().removeEmbedded();
   target.uri().remove(p_method);

   // !jf! this code assumes you have a UserProfile
   auto inv = makeNewSession(new InviteSessionCreator(*this,
                                                      target,
                                                      userProfile,
                                                      initialOffer, level, alternative, serverSub), appDs);
   DumHelper::setOutgoingEncryptionLevel(*inv, level);

   //could pass dummy target, then apply merge rules from 19.1.5...or
   //makeNewSession would use rules from 19.1.5
   if (refer.exists(h_ReferredBy))
   {
      inv->header(h_ReferredBy) = refer.header(h_ReferredBy);
   }

   const Uri& referTo = refer.header(h_ReferTo).uri();
   //19.1.5
   if (referTo.hasEmbedded() && referTo.embedded().exists(h_Replaces))
   {
      inv->header(h_Replaces) = referTo.embedded().header(h_Replaces);
   }

   return inv;
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeRefer(const NameAddr& target, const std::shared_ptr<UserProfile>& userProfile, const H_ReferTo::Type& referTo, AppDialogSet* appDs)
{
   return makeNewSession(new SubscriptionCreator(*this, target, userProfile, referTo), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeRefer(const NameAddr& target, const H_ReferTo::Type& referTo, AppDialogSet* appDs)
{
   return makeNewSession(new SubscriptionCreator(*this, target, getMasterUserProfile(), referTo), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeSubscription(const NameAddr& target, const std::shared_ptr<UserProfile>& userProfile, const Data& eventType, AppDialogSet* appDs)
{
   resip_assert(userProfile.get());
   return makeNewSession(new SubscriptionCreator(*this, target, userProfile, eventType, userProfile->getDefaultSubscriptionTime()), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeSubscription(const NameAddr& target, const std::shared_ptr<UserProfile>& userProfile, const Data& eventType,
                                     uint32_t subscriptionTime, AppDialogSet* appDs)
{
   return makeNewSession(new SubscriptionCreator(*this, target, userProfile, eventType, subscriptionTime), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeSubscription(const NameAddr& target, const std::shared_ptr<UserProfile>& userProfile, const Data& eventType,
                                     uint32_t subscriptionTime, int refreshInterval, AppDialogSet* appDs)
{
   return makeNewSession(new SubscriptionCreator(*this, target, userProfile, eventType, subscriptionTime, refreshInterval), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeSubscription(const NameAddr& target, const Data& eventType, AppDialogSet* appDs)
{
   return makeNewSession(new SubscriptionCreator(*this, target, getMasterUserProfile(), eventType, getMasterProfile()->getDefaultSubscriptionTime()), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeSubscription(const NameAddr& target, const Data& eventType,
                                     uint32_t subscriptionTime, AppDialogSet* appDs)
{
   return makeNewSession(new SubscriptionCreator(*this, target, getMasterUserProfile(), eventType, subscriptionTime), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeSubscription(const NameAddr& target, const Data& eventType,
                                     uint32_t subscriptionTime, int refreshInterval, AppDialogSet* appDs)
{
   return makeNewSession(new SubscriptionCreator(*this, target, getMasterUserProfile(), eventType, subscriptionTime, refreshInterval), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeRegistration(const NameAddr& target, const std::shared_ptr<UserProfile>& userProfile, AppDialogSet* appDs)
{
   resip_assert(userProfile.get());
   return makeNewSession(new RegistrationCreator(*this, target, userProfile, userProfile->getDefaultRegistrationTime()), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeRegistration(const NameAddr& target, const std::shared_ptr<UserProfile>& userProfile, uint32_t registrationTime, AppDialogSet* appDs)
{
   return makeNewSession(new RegistrationCreator(*this, target, userProfile, registrationTime), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeRegistration(const NameAddr& target, AppDialogSet* appDs)
{
   return makeNewSession(new RegistrationCreator(*this, target, getMasterUserProfile(), getMasterProfile()->getDefaultRegistrationTime()), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeRegistration(const NameAddr& target, uint32_t registrationTime, AppDialogSet* appDs)
{
   return makeNewSession(new RegistrationCreator(*this, target, getMasterUserProfile(), registrationTime), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makePublication(const NameAddr& targetDocument,
                                    const std::shared_ptr<UserProfile>& userProfile,
                                    const Contents& body,
                                    const Data& eventType,
                                    uint32_t expiresSeconds,
                                    AppDialogSet* appDs)
{
   return makeNewSession(new PublicationCreator(*this, targetDocument, userProfile, body, eventType, expiresSeconds), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makePublication(const NameAddr& targetDocument,
                                    const Contents& body,
                                    const Data& eventType,
                                    uint32_t expiresSeconds,
                                    AppDialogSet* appDs)
{
   return makeNewSession(new PublicationCreator(*this, targetDocument, getMasterUserProfile(), body, eventType, expiresSeconds), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeOutOfDialogRequest(const NameAddr& target, const std::shared_ptr<UserProfile>& userProfile, const MethodTypes meth, AppDialogSet* appDs)
{
   return makeNewSession(new OutOfDialogReqCreator(*this, meth, target, userProfile), appDs);
}

std::shared_ptr<SipMessage>
DialogUsageManager::makeOutOfDialogRequest(const NameAddr& target, const MethodTypes meth, AppDialogSet* appDs)
{
   return makeNewSession(new OutOfDialogReqCreator(*this, meth, target, getMasterUserProfile()), appDs);
}

ClientPagerMessageHandle
DialogUsageManager::makePagerMessage(const NameAddr& target, const std::shared_ptr<UserProfile>& userProfile, AppDialogSet* appDs)
{
   if (!mClientPagerMessageHandler)
   {
      throw DumException("Cannot send MESSAGE messages without a ClientPagerMessageHandler", __FILE__, __LINE__);
   }
   DialogSet* ds = makeUacDialogSet(new PagerMessageCreator(*this, target, userProfile), appDs);
   ClientPagerMessage* cpm = new ClientPagerMessage(*this, *ds);
   ds->mClientPagerMessage = cpm;
   return cpm->getHandle();
}

ClientPagerMessageHandle
DialogUsageManager::makePagerMessage(const NameAddr& target, AppDialogSet* appDs)
{
   return makePagerMessage(target, getMasterUserProfile(), appDs);
}

void
DialogUsageManager::send(std::shared_ptr<SipMessage> msg)
{
   // !slg! There is probably a more efficient way to get the userProfile here (pass it in?)
   DialogSet* ds = findDialogSet(DialogSetId(*msg));
   UserProfile* userProfile;
   if (ds == 0)
   {
      userProfile = getMasterUserProfile().get();
   }
   else
   {
      userProfile = ds->getUserProfile().get();
   }

   resip_assert(userProfile);
   if (!userProfile->isAnonymous() && userProfile->hasUserAgent())
   {
      msg->header(h_UserAgent).value() = userProfile->getUserAgent();
   }
   if (userProfile->isAnonymous())
   {
      msg->remove(h_ReplyTo);
      msg->remove(h_UserAgent);
      msg->remove(h_Organization);
      msg->remove(h_Server);
      msg->remove(h_Subject);
      msg->remove(h_InReplyTo);

      msg->remove(h_CallInfos);
      msg->remove(h_Warnings);
   }
   
   resip_assert(userProfile);
   if (msg->isRequest() 
       && userProfile->hasProxyRequires() 
       && msg->header(h_RequestLine).method() != ACK 
       && msg->header(h_RequestLine).method() != CANCEL)
   {
      msg->header(h_ProxyRequires) = userProfile->getProxyRequires();
   }
   
   // .bwc. This is to avoid leaving extra copies of the decorator in msg,
   // when the caller of this function holds onto the reference (and this
   // happens quite often in DUM). I would prefer to refactor such that we
   // are operating on a copy in this function, but this would require a lot
   // of work on the DumFeatureChain stuff (or, require an extra copy on top 
   // of the one we're doing when we send the message to the stack, which
   // would chew up a lot of extra cycles).
   msg->clearOutboundDecorators();

   // Add outbound decorator from userprofile - note:  it is important that this is
   // done before the call to mClientAuthManager->addAuthentication, since the ClientAuthManager
   // will install outbound decorators and we want these to run after the user provided ones, in
   // case a user provided decorator modifies the message body used in auth.
   const auto outboundDecorator = userProfile->getOutboundDecorator();
   if (outboundDecorator)
   {
      msg->addOutboundDecorator(std::unique_ptr<MessageDecorator>(outboundDecorator->clone()));
   }

   if (msg->isRequest())
   {
      // We may not need to call reset() if makeRequest is always used.
      if (msg->header(h_RequestLine).method() != CANCEL &&
          msg->header(h_RequestLine).method() != ACK &&
          msg->exists(h_Vias))
      {
         msg->header(h_Vias).front().param(p_branch).reset();
      }

      if (msg->exists(h_Vias))
      {
         if(userProfile->getRportEnabled())
         {
            msg->header(h_Vias).front().param(p_rport);
         }
         else
         {
            msg->header(h_Vias).front().remove(p_rport);
         }
         const int fixedTransportPort = userProfile->getFixedTransportPort();
         if(fixedTransportPort != 0)
         {
            msg->header(h_Vias).front().sentPort() = fixedTransportPort;
         }
         const Data& fixedTransportInterface = userProfile->getFixedTransportInterface();
         if(!fixedTransportInterface.empty())
         {
            msg->header(h_Vias).front().sentHost() = fixedTransportInterface;
         }
      }

      if (mClientAuthManager.get() && msg->header(h_RequestLine).method() != ACK)
      {
         mClientAuthManager->addAuthentication(*msg);
      }

      if (msg->header(h_RequestLine).method() == INVITE)
      {
         if (ds != 0)
         {
            if (mDialogEventStateManager)
            {
               Dialog* d = ds->findDialog(*msg);
               if (d == 0)
               {
                  // If we don't have a dialog yet and we are sending an INVITE, this is a new outbound (UAC) INVITE
                  mDialogEventStateManager->onTryingUac(*ds, *msg);
               }
            }
         }
      }
   }

   DebugLog (<< "SEND: " << std::endl << std::endl << *msg);

   OutgoingEvent* event = new OutgoingEvent(msg);
   outgoingProcess(std::unique_ptr<Message>(event));
}

void 
DialogUsageManager::sendCommand(std::shared_ptr<SipMessage> request)
{
   SendCommand* s=new SendCommand(request, *this);
   post(s);
}


void DialogUsageManager::outgoingProcess(std::unique_ptr<Message> message)
{
   Data tid = Data::Empty;
   {
      OutgoingEvent* sipMsg = dynamic_cast<OutgoingEvent*>(message.get());
      if (sipMsg)
      {
         tid = sipMsg->getTransactionId();
      }
      
      DumFeatureMessage* featureMsg = dynamic_cast<DumFeatureMessage*>(message.get());
      if (featureMsg)
      {
         InfoLog(<<"Got a DumFeatureMessage" << featureMsg);
         tid = featureMsg->getTransactionId();
      }
   }

   if (tid == Data::Empty && mOutgoingMessageInterceptor.get())
   {
      mOutgoingMessageInterceptor->process(message.get());
      return;
   }
   else if (tid != Data::Empty && !mOutgoingFeatureList.empty())
   {
      FeatureChainMap::iterator it;
      //efficiently find or create FeatureChain, should prob. be a utility template
      {
         FeatureChainMap::iterator lb = mOutgoingFeatureChainMap.lower_bound(tid);
         if (lb != mOutgoingFeatureChainMap.end() && !(mOutgoingFeatureChainMap.key_comp()(tid, lb->first)))
         {
            it = lb;
         }
         else
         {
            it = mOutgoingFeatureChainMap.insert(lb, FeatureChainMap::value_type(tid, new DumFeatureChain(*this, mOutgoingFeatureList, *mOutgoingTarget)));
         }
      }
      
      DumFeatureChain::ProcessingResult res = it->second->process(message.get());
      
      if (res & DumFeatureChain::ChainDoneBit)
      {
         delete it->second;
         mOutgoingFeatureChainMap.erase(it);
      }

      if (res & DumFeatureChain::EventTakenBit)
      {
         message.release();
         return;
      }
   }

   OutgoingEvent* event = dynamic_cast<OutgoingEvent*>(message.get());
   //resip_assert(event);
   //.dcm. a TID collision can cause a message to be delivered to a finished
   //chain. This is probably because pseudorandom was being used on Win32.
   if (event)
   {
      if (event->message()->isRequest())
      {
         DialogSet* ds = findDialogSet(DialogSetId(*event->message()));
         UserProfile* userProfile;
         if (ds == 0)
         {
            userProfile = getMasterUserProfile().get();
         }
         else
         {
            userProfile = ds->getUserProfile().get();
         }

         resip_assert(userProfile);

         //!dcm! -- unique SharedPtr to unique_ptr conversion prob. a worthwhile
         //optimzation here. SharedPtr would have to be changed; would
         //throw/assert if not unique.
         std::unique_ptr<SipMessage> toSend(static_cast<SipMessage*>(event->message()->clone()));

         // .bwc. Protect ourselves from garbage with an isWellFormed() check.
         // (Code in Dialog doesn't check for well-formedness in the 
         // Record-Route stack, so bad stuff there can end up here)
         if (event->message()->exists(h_Routes) &&
             !event->message()->header(h_Routes).empty() &&
             event->message()->header(h_Routes).front().isWellFormed() &&
             !event->message()->header(h_Routes).front().uri().exists(p_lr))
         {
            Helper::processStrictRoute(*toSend);
            sendUsingOutboundIfAppropriate(*userProfile, std::move(toSend));
         }
         else
         {
            sendUsingOutboundIfAppropriate(*userProfile, std::move(toSend));
         }
      }
      else
      {
         sendResponse(*event->message());
      }
   }
}

void
DialogUsageManager::sendUsingOutboundIfAppropriate(UserProfile& userProfile, std::unique_ptr<SipMessage> msg)
{
   //a little inefficient, branch parameter might be better
   DialogId id(*msg);
   if (userProfile.hasOutboundProxy() && 
      (!findDialog(id) || userProfile.getForceOutboundProxyOnAllRequestsEnabled()))
   {
      if (userProfile.getExpressOutboundAsRouteSetEnabled())
      {
         // prepend the outbound proxy to the service route
         msg->header(h_Routes).push_front(NameAddr(userProfile.getOutboundProxy().uri()));
         if(userProfile.clientOutboundEnabled() && userProfile.mClientOutboundFlowTuple.mFlowKey != 0)
         {
            DebugLog(<< "Sending with OutboundProxy=" << userProfile.getOutboundProxy().uri() << " in route set, to FlowTuple=" << userProfile.mClientOutboundFlowTuple << ", FlowKey=" << userProfile.mClientOutboundFlowTuple.mFlowKey << ": " << msg->brief());
            mStack.sendTo(std::move(msg), userProfile.mClientOutboundFlowTuple, this);
         }
         else
         {
            DebugLog(<< "Sending with OutboundProxy=" << userProfile.getOutboundProxy().uri() << " in route set: " << msg->brief());
            mStack.send(std::move(msg), this);
         }
      }
      else
      {
         if(userProfile.clientOutboundEnabled() && userProfile.mClientOutboundFlowTuple.mFlowKey != 0)
         {
            DebugLog(<< "Sending to FlowTuple=" << userProfile.mClientOutboundFlowTuple << ", FlowKey=" << userProfile.mClientOutboundFlowTuple.mFlowKey << ": " << msg->brief());
            mStack.sendTo(std::move(msg), userProfile.mClientOutboundFlowTuple, this);
         }
         else
         {
            DebugLog(<< "Sending to OutboundProxy=" << userProfile.getOutboundProxy().uri() << ": " << msg->brief());
            mStack.sendTo(std::move(msg), userProfile.getOutboundProxy().uri(), this);
         }
      }
   }
   else
   {
      if(userProfile.clientOutboundEnabled() && userProfile.mClientOutboundFlowTuple.mFlowKey != 0)
      {
         DebugLog(<< "Sending to FlowTuple=" << userProfile.mClientOutboundFlowTuple << ", FlowKey=" << userProfile.mClientOutboundFlowTuple.mFlowKey << ": " << msg->brief());
         mStack.sendTo(std::move(msg), userProfile.mClientOutboundFlowTuple, this);
      }
      else
      {
         DebugLog(<< "Send: " << msg->brief());
         mStack.send(std::move(msg), this);
      }
   }
}


void
DialogUsageManager::end(DialogSetId setid)
{
   DialogSet* ds = findDialogSet(setid);
   if (ds == 0)
   {
      throw Exception("Request no longer exists", __FILE__, __LINE__);
   }
   else
   {
      ds->end();
   }
}

void
DialogUsageManager::destroy(const BaseUsage* usage)
{
   if (mShutdownState != Destroying)
   {
      post(new DestroyUsage(usage->mHandle));
   }
   else
   {
      InfoLog(<< "DialogUsageManager::destroy() not posting to stack");
   }
}

void
DialogUsageManager::destroy(DialogSet* dset)
{
   if (mShutdownState != Destroying)
   {
      post(new DestroyUsage(dset));
   }
   else
   {
      InfoLog(<< "DialogUsageManager::destroy() not posting to stack");
   }
}

void
DialogUsageManager::destroy(Dialog* d)
{
   if (mShutdownState != Destroying)
   {
      post(new DestroyUsage(d));
   }
   else
   {
      InfoLog(<< "DialogUsageManager::destroy() not posting to stack");
   }
}


Dialog*
DialogUsageManager::findDialog(const DialogId& id)
{
   DialogSet* ds = findDialogSet(id.getDialogSetId());
   if (ds)
   {
      return ds->findDialog(id);
   }
   else
   {
      return 0;
   }
}


InviteSessionHandle
DialogUsageManager::findInviteSession(const DialogId& id)
{
   Dialog* dialog = findDialog(id);
   if (dialog && dialog->mInviteSession)
   {
      return dialog->mInviteSession->getSessionHandle();
   }

   return InviteSessionHandle::NotValid();
}

pair<InviteSessionHandle, int>
DialogUsageManager::findInviteSession(const CallId& replaces)
{
   //486/481/603 decision making logic where?  App may not wish to keep track of
   //invitesession state
   //Logic is here for now.
   InviteSessionHandle is = findInviteSession(DialogId(replaces.value(),
                                                       replaces.param(p_toTag),
                                                       replaces.param(p_fromTag)));
   int ErrorStatusCode = 481; // Call/Transaction Does Not Exist

   // If we matched a session - Do RFC3891 Section 3 Processing
   if(is.isValid())
   {
      // Note some missing checks are:
      // 1.  If the Replaces header field matches more than one dialog, the UA must act as
      //     if no match was found
      // 2.  Verify that the initiator of the new Invite is authorized
      if(is->isTerminated())
      {
         ErrorStatusCode = 603; // Declined
         is = InviteSessionHandle::NotValid();
      }
      else if(is->isConnected())
      {
         // Check if early-only flag is present in replaces header
         if(replaces.exists(p_earlyOnly))
         {
            ErrorStatusCode = 486; // Busy Here
            is = InviteSessionHandle::NotValid();
         }
      }      
      else if(!is->isEarly())
      {
         // replaces can't be used on early dialogs that were not initiated by this UA - ie. InviteSession::Proceeding state
         ErrorStatusCode = 481; // Call/Transaction Does Not Exist
         is = InviteSessionHandle::NotValid();
      }
   }
   return make_pair(is, ErrorStatusCode);
}

AppDialogHandle DialogUsageManager::findAppDialog(const DialogId& id)
{
   Dialog* pDialog = findDialog(id);

   if(pDialog && pDialog->mAppDialog)
   {
      return pDialog->mAppDialog->getHandle();
   }

   // Return an invalid handle
   return AppDialogHandle();
}

AppDialogSetHandle DialogUsageManager::findAppDialogSet(const DialogSetId& id)
{
   DialogSet* pDialogSet = findDialogSet(id);

   if(pDialogSet && pDialogSet->mAppDialogSet)
   {
      return pDialogSet->mAppDialogSet->getHandle();
   }

   // Return an invalid handle
   return AppDialogSetHandle();
}

void
DialogUsageManager::internalProcess(std::unique_ptr<Message> msg)
{
#ifdef RESIP_DUM_THREAD_DEBUG
   if(!mThreadDebugKey)
   {
      // .bwc. Probably means multiple threads are trying to give DUM cycles 
      // simultaneously.
      resip_assert(!mHiddenThreadDebugKey);
      // No d'tor needed, since we're just going to use a pointer to this.
      if(!ThreadIf::tlsKeyCreate(mThreadDebugKey, 0))
      {
         // .bwc. We really could pass anything here, but for the sake of 
         // passing a valid pointer, I have (completely arbitrarily) chosen a 
         // pointer to the DUM. All that matters is that this value is non-null
         ThreadIf::tlsSetValue(mThreadDebugKey, this);
      }
      else
      {
         ErrLog(<< "ThreadIf::tlsKeyCreate() failed!");
      }
   }
#endif

   threadCheck();

   // After a Stack ShutdownMessage has been received, don't do anything else in dum
   if (mShutdownState == Shutdown)
   {
      return;
   }

   {
      TransactionUserMessage* tuMsg = dynamic_cast<TransactionUserMessage*>(msg.get());
      if (tuMsg)
      {
         InfoLog (<< "TU unregistered ");
         resip_assert(mShutdownState == RemovingTransactionUser);
         resip_assert(tuMsg->type() == TransactionUserMessage::TransactionUserRemoved);
         mShutdownState = Shutdown;
         if (mDumShutdownHandler)
         {
            mDumShutdownHandler->onDumCanBeDeleted();
            mDumShutdownHandler = 0; // prevent mDumShutdownHandler getting called more than once
         }
         return;
      }
   }
   
   {
      KeepAlivePong* pong = dynamic_cast<KeepAlivePong*>(msg.get());
      if (pong)
      {
         DebugLog(<< "keepalive pong received from " << pong->getFlow());
         if (mKeepAliveManager.get())
         {
            mKeepAliveManager->receivedPong(pong->getFlow());
         }
         return;
      }
   }

   {
      DestroyUsage* destroyUsage = dynamic_cast<DestroyUsage*>(msg.get());
      if (destroyUsage)
      {
         //DebugLog(<< "Destroying usage" );
         destroyUsage->destroy();
         return;
      }
   }

   {
      DumTimeout* dumMsg = dynamic_cast<DumTimeout*>(msg.get());
      if (dumMsg)
      {
         //DebugLog(<< "Timeout Message" );
         if (!dumMsg->getBaseUsage().isValid())
         {
            return;
         }
         dumMsg->getBaseUsage()->dispatch(*dumMsg);
         return;
      }
   }

   {
      KeepAliveTimeout* keepAliveMsg = dynamic_cast<KeepAliveTimeout*>(msg.get());
      if (keepAliveMsg)
      {
         //DebugLog(<< "Keep Alive Message" );
         if (mKeepAliveManager.get())
         {
            mKeepAliveManager->process(*keepAliveMsg);
         }
         return;
      }
   }

   {
      KeepAlivePongTimeout* keepAlivePongMsg = dynamic_cast<KeepAlivePongTimeout*>(msg.get());
      if (keepAlivePongMsg)
      {
         //DebugLog(<< "Keep Alive Pong Message" );
         if (mKeepAliveManager.get())
         {
            mKeepAliveManager->process(*keepAlivePongMsg);
         }
         return;
      }
   }

   {
      ConnectionTerminated* terminated = dynamic_cast<ConnectionTerminated*>(msg.get());
      if (terminated)
      {
         // Notify all dialogSets, in case they need to react (ie. client outbound support)
         // First find all applicable dialogsets, since flow token in user profile will 
         // be cleared by first dialogset we notify, then notify all dialogset's
         std::list<DialogSet*> dialogSetsToNotify;
         DialogSetMap::iterator it =  mDialogSetMap.begin();
         for(; it != mDialogSetMap.end(); it++)
         {
            if(it->second->mUserProfile->clientOutboundEnabled() && 
               it->second->mUserProfile->getClientOutboundFlowTuple().mFlowKey == terminated->getFlow().mFlowKey &&  // Flow key is not part of Tuple operator=, check it first
               it->second->mUserProfile->getClientOutboundFlowTuple() == terminated->getFlow())
            {
               if(it->second->getClientRegistration().isValid())
               {
                   // ensure client registrations are notified first
                   dialogSetsToNotify.push_front(it->second);
               }
               else
               {
                  dialogSetsToNotify.push_back(it->second);
               }
            }
         }
         // Now dispatch notification to all dialogsets found above
         std::list<DialogSet*>::iterator it2 = dialogSetsToNotify.begin();
         for(; it2 != dialogSetsToNotify.end();it2++)
         {
            (*it2)->flowTerminated(terminated->getFlow());
         }

         DebugLog(<< *msg);
         if (mConnectionTerminatedEventDispatcher.dispatch(msg.get()))
         {
            // If EventDispatcher returns true, then it took ownership of msg, so we release it
            msg.release();
         }
         return;
      }
   }

   {
      DumCommand* command = dynamic_cast<DumCommand*>(msg.get());
      if (command)
      {
         //DebugLog(<< "DumCommand" );
         command->executeCommand();
         return;
      }
   }

   {
      ExternalMessageBase* externalMessage = dynamic_cast<ExternalMessageBase*>(msg.get());
      if (externalMessage)
      {
         processExternalMessage(externalMessage);
         return;
      }
   }

   incomingProcess(std::move(msg));
}

void
DialogUsageManager::processExternalMessage(ExternalMessageBase* externalMessage)
{
   bool handled = false;
   for(std::vector<ExternalMessageHandler*>::iterator i = mExternalMessageHandlers.begin(); 
      i != mExternalMessageHandlers.end(); ++i)
   {
      (*i)->onMessage(externalMessage, handled);
      if (handled)
      {
         break;
      }
   }
}

void 
DialogUsageManager::incomingProcess(std::unique_ptr<Message> msg)
{
   //call or create feature chain if appropriate
   Data tid = Data::Empty;
   {
      SipMessage* sipMsg = dynamic_cast<SipMessage*>(msg.get());
      if (sipMsg)
      {
         tid = sipMsg->getTransactionId();
         bool garbage=false;
         Data reason;

         if(!sipMsg->header(h_From).isWellFormed())
         {
            garbage=true;
            reason.append("Malformed From, ",16);
         }

         if(!sipMsg->header(h_To).isWellFormed())
         {
            garbage=true;
            reason.append("Malformed To, ",14);
         }

         if(!sipMsg->header(h_CallId).isWellFormed())
         {
            garbage=true;
            reason.append("Malformed Call-Id, ",19);
         }
         
         if(garbage)
         {
            if(sipMsg->isRequest() && sipMsg->method()!=ACK)
            {
               // .bwc. Either we need to trim the last comma off, or make this 
               // a proper sentence fragment. This is more fun.
               reason.append("fix your code!",14);
               SipMessage failure;
               makeResponse(failure, *sipMsg, 400, reason);
               sendResponse(failure);
            }

            InfoLog (<< "Malformed header in message (" << reason << ") - rejecting/discarding: " << *sipMsg);
            
            // .bwc. Only forge a response when appropriate, but return in any 
            // case.
            return;
         }
      }
      
      DumFeatureMessage* featureMsg = dynamic_cast<DumFeatureMessage*>(msg.get());
      if (featureMsg)
      {
         //DebugLog(<<"Got a DumFeatureMessage" << featureMsg);
         tid = featureMsg->getTransactionId();
      }
   }
   if (tid != Data::Empty && !mIncomingFeatureList.empty())
   {
      FeatureChainMap::iterator it;
      //efficiently find or create FeatureChain, should prob. be a utility template
      {
         FeatureChainMap::iterator lb = mIncomingFeatureChainMap.lower_bound(tid);
         if (lb != mIncomingFeatureChainMap.end() && !(mIncomingFeatureChainMap.key_comp()(tid, lb->first)))
         {
            it = lb;
         }
         else
         {
            if(dynamic_cast<SipMessage*>(msg.get()))
            {
               it = mIncomingFeatureChainMap.insert(lb, FeatureChainMap::value_type(tid, new DumFeatureChain(*this, mIncomingFeatureList, *mIncomingTarget)));
            }
            else
            {
               // Certain messages from the wire (ie: CANCEL) can end a feature, however there may still be some 
               // pending Async requests (non-SipMessages) that are coming in - just drop them if so
               return;
            }
         }
      }
      
      DumFeatureChain::ProcessingResult res = it->second->process(msg.get());
      
      if (res & DumFeatureChain::ChainDoneBit)
      {
         delete it->second;
         mIncomingFeatureChainMap.erase(it);
         //DebugLog(<< "feature chain deleted" << endl);
      }
 
      if (res & DumFeatureChain::EventTakenBit)
      {
         msg.release();
         //DebugLog(<< "event taken");
         return;
      }
   }
   
   try
   {
      DebugLog (<< "Got: " << msg->brief());
      DumDecrypted* decryptedMsg = dynamic_cast<DumDecrypted*>(msg.get());
      SipMessage* sipMsg = 0;
      if (decryptedMsg)
      {
         sipMsg = decryptedMsg->decrypted();
      }
      else
      {
         sipMsg = dynamic_cast<SipMessage*>(msg.get());
      }

      if (sipMsg)
      {
         DebugLog ( << "DialogUsageManager::process: found SipMessage" );
         if (sipMsg->isRequest())
         {
            // Validate Request URI
            if( !validateRequestURI(*sipMsg) )
            {
               DebugLog (<< "Failed RequestURI validation " << *sipMsg);
               return;
            }

            // Continue validation on all requests, except ACK and CANCEL
            if(sipMsg->header(h_RequestLine).method() != ACK &&
               sipMsg->header(h_RequestLine).method() != CANCEL)
            {
               if( !validateRequiredOptions(*sipMsg) )
               {
                  DebugLog (<< "Failed required options validation " << *sipMsg);
                  return;
               }
               if( !validate100RelSupport(*sipMsg) )
               {
                  DebugLog (<< "Remote party does not support 100rel " << *sipMsg);
                  return;
               }
               if( getMasterProfile()->validateContentEnabled() && !validateContent(*sipMsg) )
               {
                  DebugLog (<< "Failed content validation " << *sipMsg);
                  return;
               }
               if( getMasterProfile()->validateAcceptEnabled() && !validateAccept(*sipMsg) )
               {
                  DebugLog (<< "Failed accept validation " << *sipMsg);
                  return;
               }
            }
            if (sipMsg->header(h_From).exists(p_tag))
            {
               if (mergeRequest(*sipMsg) )
               {
                  InfoLog (<< "Merged request: " << *sipMsg);
                  return;
               }
            }
            processRequest(*sipMsg);
         }
         else
         {
            processResponse(*sipMsg);
         }
      }
   }
   catch(BaseException& e)
   {
      //unparseable, bad 403 w/ 2543 trans it from FWD, etc
     ErrLog(<<"Illegal message rejected: " << e.getMessage());
   }
}

bool 
DialogUsageManager::hasEvents() const
{
   return mFifo.messageAvailable();
}

// return true if there is more to do
bool 
DialogUsageManager::process(resip::Mutex* mutex)
{
   if (mFifo.messageAvailable())
   {
      resip::PtrLock lock(mutex);
#ifdef RESIP_DUM_THREAD_DEBUG
      mThreadDebugKey=mHiddenThreadDebugKey;
#endif
      internalProcess(std::unique_ptr<Message>(mFifo.getNext()));
#ifdef RESIP_DUM_THREAD_DEBUG
      // .bwc. Thread checking is disabled if mThreadDebugKey is 0; if the app 
      // is using this mutex-locked process() call, we only enable thread-
      // checking while the mutex is locked. Accesses from another thread while 
      // the mutex is not locked are probably intentional. However, if the app 
      // accesses the DUM inappropriately anyway, we'll probably detect it if 
      // it happens during the internalProcess() call.
      mHiddenThreadDebugKey=mThreadDebugKey;
      mThreadDebugKey=0;
#endif
   }
   return mFifo.messageAvailable();
}

bool 
DialogUsageManager::process(int timeoutMs, resip::Mutex* mutex)
{
   std::unique_ptr<Message> message;

   if(timeoutMs == -1)
   {
      message.reset(mFifo.getNext());
   }
   else
   {
      message.reset(mFifo.getNext(timeoutMs));
   }
   if (message.get())
   {
      resip::PtrLock lock(mutex);
#ifdef RESIP_DUM_THREAD_DEBUG
      mThreadDebugKey=mHiddenThreadDebugKey;
#endif
      internalProcess(std::move(message));
#ifdef RESIP_DUM_THREAD_DEBUG
      // .bwc. Thread checking is disabled if mThreadDebugKey is 0; if the app 
      // is using this mutex-locked process() call, we only enable thread-
      // checking while the mutex is locked. Accesses from another thread while 
      // the mutex is not locked are probably intentional. However, if the app 
      // accesses the DUM inappropriately anyway, we'll probably detect it if 
      // it happens during the internalProcess() call.
      mHiddenThreadDebugKey=mThreadDebugKey;
      mThreadDebugKey=0;
#endif
   }
   return mFifo.messageAvailable();
}

// return true if there is more to do
bool
DialogUsageManager::process(resip::RecursiveMutex& mutex)
{
   if (mFifo.messageAvailable())
   {
      resip::RecursiveLock lock(mutex);
#ifdef RESIP_DUM_THREAD_DEBUG
      mThreadDebugKey = mHiddenThreadDebugKey;
#endif
      internalProcess(std::unique_ptr<Message>(mFifo.getNext()));
#ifdef RESIP_DUM_THREAD_DEBUG
      // .bwc. Thread checking is disabled if mThreadDebugKey is 0; if the app 
      // is using this mutex-locked process() call, we only enable thread-
      // checking while the mutex is locked. Accesses from another thread while 
      // the mutex is not locked are probably intentional. However, if the app 
      // accesses the DUM inappropriately anyway, we'll probably detect it if 
      // it happens during the internalProcess() call.
      mHiddenThreadDebugKey = mThreadDebugKey;
      mThreadDebugKey = 0;
#endif
   }
   return mFifo.messageAvailable();
}

bool
DialogUsageManager::process(int timeoutMs, resip::RecursiveMutex& mutex)
{
   std::unique_ptr<Message> message;

   if (timeoutMs == -1)
   {
      message.reset(mFifo.getNext());
   }
   else
   {
      message.reset(mFifo.getNext(timeoutMs));
   }
   if (message.get())
   {
      resip::RecursiveLock lock(mutex);
#ifdef RESIP_DUM_THREAD_DEBUG
      mThreadDebugKey = mHiddenThreadDebugKey;
#endif
      internalProcess(std::move(message));
#ifdef RESIP_DUM_THREAD_DEBUG
      // .bwc. Thread checking is disabled if mThreadDebugKey is 0; if the app 
      // is using this mutex-locked process() call, we only enable thread-
      // checking while the mutex is locked. Accesses from another thread while 
      // the mutex is not locked are probably intentional. However, if the app 
      // accesses the DUM inappropriately anyway, we'll probably detect it if 
      // it happens during the internalProcess() call.
      mHiddenThreadDebugKey = mThreadDebugKey;
      mThreadDebugKey = 0;
#endif
   }
   return mFifo.messageAvailable();
}

bool
DialogUsageManager::validateRequestURI(const SipMessage& request)
{
   // RFC3261 - 8.2.1
   if (!getMasterProfile()->isMethodSupported(request.header(h_RequestLine).getMethod()))
   {
      InfoLog (<< "Received an unsupported method: " << request.brief());

      SipMessage failure;
      makeResponse(failure, request, 405);
      failure.header(h_Allows) = getMasterProfile()->getAllowedMethods();
      sendResponse(failure);

      if(mRequestValidationHandler)
         mRequestValidationHandler->onInvalidMethod(request);

      return false;
   }

   // RFC3261 - 8.2.2
   if (!getMasterProfile()->isSchemeSupported(request.header(h_RequestLine).uri().scheme()))
   {
      InfoLog (<< "Received an unsupported scheme: " << request.brief());
      SipMessage failure;
      makeResponse(failure, request, 416);
      sendResponse(failure);
      
      if(mRequestValidationHandler)
         mRequestValidationHandler->onInvalidScheme(request);

      return false;
   }

   return true;
}


bool
DialogUsageManager::validateRequiredOptions(const SipMessage& request)
{
   // RFC 2162 - 8.2.2
   if(request.exists(h_Requires) &&                 // Don't check requires if method is ACK or CANCEL
      request.header(h_RequestLine).getMethod() != ACK &&
      request.header(h_RequestLine).getMethod() != CANCEL)
   {
      Tokens unsupported = getMasterProfile()->getUnsupportedOptionsTags(request.header(h_Requires));
      if (!unsupported.empty())
      {
         InfoLog (<< "Received an unsupported option tag(s): " << request.brief());

         SipMessage failure;
         makeResponse(failure, request, 420);
         failure.header(h_Unsupporteds) = unsupported;
         sendResponse(failure);
      
         if(mRequestValidationHandler)
            mRequestValidationHandler->onInvalidRequiredOptions(request);

         return false;
      }
   }

   return true;
}


bool
DialogUsageManager::validate100RelSupport(const SipMessage& request)
{
   if(request.header(h_RequestLine).getMethod() == INVITE)
   {
      if (getMasterProfile()->getUasReliableProvisionalMode() == MasterProfile::Required)
      {
         if (!((request.exists(h_Requires) && request.header(h_Requires).find(Token(Symbols::C100rel)))
               || (request.exists(h_Supporteds) && request.header(h_Supporteds).find(Token(Symbols::C100rel)))))
         {
            SipMessage failure;
            makeResponse(failure, request, 421);
            failure.header(h_Requires).push_back(Token(Symbols::C100rel));
            sendResponse(failure);
      
            if(mRequestValidationHandler)
            {
               mRequestValidationHandler->on100RelNotSupportedByRemote(request);
            }

            return false;
         }
      }
   }
   return true;
}

bool
DialogUsageManager::validateContent(const SipMessage& request)
{
   // RFC3261 - 8.2.3
   // Don't need to validate content headers if they are specified as optional in the content-disposition
   if (!(request.exists(h_ContentDisposition) &&
         request.header(h_ContentDisposition).isWellFormed() &&
        request.header(h_ContentDisposition).exists(p_handling) &&
        isEqualNoCase(request.header(h_ContentDisposition).param(p_handling), Symbols::Optional)))
   {
     if (request.exists(h_ContentType) && !getMasterProfile()->isMimeTypeSupported(request.header(h_RequestLine).method(), request.header(h_ContentType)))
      {
         InfoLog (<< "Received an unsupported mime type: " << request.header(h_ContentType) << " for " << request.brief());

         SipMessage failure;
         makeResponse(failure, request, 415);
         failure.header(h_Accepts) = getMasterProfile()->getSupportedMimeTypes(request.header(h_RequestLine).method());
         sendResponse(failure);
            
         if(mRequestValidationHandler)
            mRequestValidationHandler->onInvalidContentType(request);

         return false;
      }

     if (request.exists(h_ContentEncoding) && !getMasterProfile()->isContentEncodingSupported(request.header(h_ContentEncoding)))
      {
         InfoLog (<< "Received an unsupported mime type: " << request.header(h_ContentEncoding) << " for " << request.brief());
         SipMessage failure;
         makeResponse(failure, request, 415);
         failure.header(h_AcceptEncodings) = getMasterProfile()->getSupportedEncodings();
         sendResponse(failure);
         
         if(mRequestValidationHandler)
            mRequestValidationHandler->onInvalidContentEncoding(request);

         return false;
      }

      if (getMasterProfile()->validateContentLanguageEnabled() &&
          request.exists(h_ContentLanguages) && !getMasterProfile()->isLanguageSupported(request.header(h_ContentLanguages)))
      {
         InfoLog (<< "Received an unsupported language: " << request.header(h_ContentLanguages).front() << " for " << request.brief());

         SipMessage failure;
         makeResponse(failure, request, 415);
         failure.header(h_AcceptLanguages) = getMasterProfile()->getSupportedLanguages();
         sendResponse(failure);
         
         if(mRequestValidationHandler)
            mRequestValidationHandler->onInvalidContentLanguage(request);

         return false;
      }
   }

   return true;
}

bool
DialogUsageManager::validateAccept(const SipMessage& request)
{
   MethodTypes method = request.header(h_RequestLine).method();
   // checks for Accept to comply with SFTF test case 216
   if(request.exists(h_Accepts))
   {
      for (Mimes::const_iterator i = request.header(h_Accepts).begin();
           i != request.header(h_Accepts).end(); i++)
      {
         if (getMasterProfile()->isMimeTypeSupported(method, *i))
         {
            return true;  // Accept header passes validation if we support as least one of the mime types
         }
      }
   }
   // If no Accept header then application/sdp should be assumed for certain methods
   else if(method == INVITE ||
           method == OPTIONS ||
           method == PRACK ||
           method == UPDATE)
   {
      if (getMasterProfile()->isMimeTypeSupported(request.header(h_RequestLine).method(), Mime("application", "sdp")))
      {
         return true;
      }
   }
   else
   {
      // Other method without an Accept Header
      return true;
   }

   InfoLog (<< "Received unsupported mime types in accept header: " << request.brief());
   SipMessage failure;
   makeResponse(failure, request, 406);
   failure.header(h_Accepts) = getMasterProfile()->getSupportedMimeTypes(method);
   sendResponse(failure);

   if(mRequestValidationHandler)
      mRequestValidationHandler->onInvalidAccept(request);

   return false;
}

bool
DialogUsageManager::mergeRequest(const SipMessage& request)
{
   resip_assert(request.isRequest());
   resip_assert(request.isExternal());

   if (!request.header(h_To).exists(p_tag))
   {
      if (mMergedRequests.find(MergedRequestKey(request, getMasterProfile()->checkReqUriInMergeDetectionEnabled())) != mMergedRequests.end())
      {
         SipMessage failure;
         makeResponse(failure, request, 482, "Merged Request");
         failure.header(h_AcceptLanguages) = getMasterProfile()->getSupportedLanguages();
         sendResponse(failure);
         return true;
      }
   }

   return false;
}

void
DialogUsageManager::processRequest(const SipMessage& request)
{
   DebugLog ( << "DialogUsageManager::processRequest: " << request.brief());

   if (mShutdownState != Running && mShutdownState != ShutdownRequested)
   {
      WarningLog (<< "Ignoring a request since we are shutting down " << request.brief());

      SipMessage failure;
      makeResponse(failure, request, 480, "UAS is shutting down");
      sendResponse(failure);
      return;
   }

   if (request.header(h_RequestLine).method() == PUBLISH)
   {
      processPublish(request);
      return;
   }

   bool toTag = request.header(h_To).exists(p_tag);
   if(request.header(h_RequestLine).getMethod() == REGISTER && toTag && getMasterProfile()->allowBadRegistrationEnabled())
   {
       toTag = false;
   }

   resip_assert(mAppDialogSetFactory.get());
   // !jf! note, the logic was reversed during ye great merge of March of Ought 5
   if (toTag ||
       findDialogSet(DialogSetId(request)))
   {
      switch (request.header(h_RequestLine).getMethod())
      {
         case REGISTER:
         {
            SipMessage failure;
            makeResponse(failure, request, 400, "Registration requests can't have To: tags.");
            failure.header(h_AcceptLanguages) = getMasterProfile()->getSupportedLanguages();
            sendResponse(failure);
            break;
         }

         default:
         {
            DialogSet* ds = findDialogSet(DialogSetId(request));
            if (ds == 0)
            {
               if (request.header(h_RequestLine).method() != ACK)
               {
                  SipMessage failure;
                  makeResponse(failure, request, 481);
                  failure.header(h_AcceptLanguages) = getMasterProfile()->getSupportedLanguages();
                  InfoLog (<< "Rejected request (which was in a dialog) " << request.brief());
                  sendResponse(failure);
               }
               else
               {
                  InfoLog (<< "ACK doesn't match any dialog" << request.brief());
               }
            }
            else
            {
               InfoLog (<< "Handling in-dialog request: " << request.brief());
               ds->dispatch(request);
            }
         }
      }
   }
   else
   {
      switch (request.header(h_RequestLine).getMethod())
      {
         case ACK:
            DebugLog (<< "Discarding request: " << request.brief());
            break;

         case PRACK:
         case BYE:
         case UPDATE:
         case INFO: // !rm! in an ideal world
         {
            SipMessage failure;
            makeResponse(failure, request, 481);
            failure.header(h_AcceptLanguages) = getMasterProfile()->getSupportedLanguages();
            sendResponse(failure);
            break;
         }
         case CANCEL:
         {
            // find the appropropriate ServerInvSession
            CancelMap::iterator i = mCancelMap.find(request.getTransactionId());
            if (i != mCancelMap.end())
            {
               i->second->dispatch(request);
            }
            else
            {
               InfoLog (<< "Received a CANCEL on a non-existent transaction: tid=" << request.getTransactionId());
               SipMessage failure;
               makeResponse(failure, request, 481);
               sendResponse(failure);
            }
            break;
         }
         case PUBLISH:
            resip_assert(false);
            return;
         case SUBSCRIBE:
            if (!checkEventPackage(request))
            {
               InfoLog (<< "Rejecting request (unsupported package) " 
                        << request.brief());
               return;
            }
           /*FALLTHRU*/
         case NOTIFY : // handle unsolicited (illegal) NOTIFYs
         case INVITE:   // new INVITE
         case REFER:    // out-of-dialog REFER
            //case INFO :    // handle non-dialog (illegal) INFOs
         case OPTIONS : // handle non-dialog OPTIONS
         case MESSAGE :
         case REGISTER:
         {
            {
               DialogSetId id(request);
               //cryptographically dangerous
               if(mDialogSetMap.find(id) != mDialogSetMap.end()) 
               {
                  // this can only happen if someone sends us a request with the same callid and from tag as one 
                  // that is in the process of destroying - since this is bad endpoint behaviour - we will 
                  // reject the request with a 400 response
                  SipMessage badrequest;
                  makeResponse(badrequest, request, 400);
                  badrequest.header(h_AcceptLanguages) = getMasterProfile()->getSupportedLanguages();
                  sendResponse(badrequest);
                  return;
               }
            }
            if (mDumShutdownHandler)
            {
               SipMessage forbidden;
               makeResponse(forbidden, request, 480);
               forbidden.header(h_AcceptLanguages) = getMasterProfile()->getSupportedLanguages();
               sendResponse(forbidden);
               return;
            }
            try
            {
               DialogSet* dset =  new DialogSet(request, *this);

               StackLog ( << "*********** Calling AppDialogSetFactory *************: " << dset->getId());
               AppDialogSet* appDs = mAppDialogSetFactory->createAppDialogSet(*this, request);
               appDs->mDialogSet = dset;
               dset->setUserProfile(appDs->selectUASUserProfile(request));
               dset->mAppDialogSet = appDs;

               StackLog ( << "************* Adding DialogSet ***************: " << dset->getId());
               //StackLog ( << "Before: " << Inserter(mDialogSetMap) );
               mDialogSetMap[dset->getId()] = dset;
               StackLog ( << "DialogSetMap: " << InserterP(mDialogSetMap) );

               dset->dispatch(request);
            }
            catch (BaseException& e)
            {
               SipMessage failure;
               makeResponse(failure, request, 400, e.getMessage());
               failure.header(h_AcceptLanguages) = getMasterProfile()->getSupportedLanguages();
               sendResponse(failure);
            }

            break;
         }
         case RESPONSE:
         case SERVICE:
            resip_assert(false);
            break;
         case UNKNOWN:
         case MAX_METHODS:
            resip_assert(false);
            break;
      }
   }
}

void
DialogUsageManager::processResponse(const SipMessage& response)
{
   if (response.header(h_CSeq).method() != CANCEL)
   {
      DialogSet* ds = findDialogSet(DialogSetId(response));

      if (ds)
      {
         DebugLog ( << "DialogUsageManager::processResponse: " << std::endl << std::endl << response.brief());
         ds->dispatch(response);
      }
      else
      {
          InfoLog (<< "Throwing away stray response: " << std::endl << std::endl << response.brief());
      }
   }
}

void
DialogUsageManager::processPublish(const SipMessage& request)
{
   if (!checkEventPackage(request))
   {
      InfoLog(<< "Rejecting request (unsupported package) " << request.brief());
      return;
   }

   if (request.exists(h_SIPIfMatch))
   {
      ServerPublications::iterator i = mServerPublications.find(request.header(h_SIPIfMatch).value());
      if (i != mServerPublications.end())
      {
         i->second->dispatch(request);
      }
      else
      {
         // Check if publication exists in PublicationDb - may have been sync'd over,
         // or exists from a restart.  In this case, fabricate a new ServerSubcription 
         // to handle this request.
         if (mPublicationPersistenceManager &&
             mPublicationPersistenceManager->documentExists(request.header(h_Event).value(), request.header(h_RequestLine).uri().getAor(), request.header(h_SIPIfMatch).value()))
         {
            ServerPublication* sp = new ServerPublication(*this, request.header(h_SIPIfMatch).value(), request);
            mServerPublications[request.header(h_SIPIfMatch).value()] = sp;
            sp->dispatch(request);
         }
         else
         {
            auto response = std::make_shared<SipMessage>();
            makeResponse(*response, request, 412);
            send(response);
         }
      }
   }
   else
   {
      Data etag = Random::getCryptoRandomHex(8);
      while (mServerPublications.find(etag) != mServerPublications.end())
      {
         etag = Random::getCryptoRandomHex(8);
      }

      if (request.getContents())
      {
         ServerPublication* sp = new ServerPublication(*this, etag, request);
         mServerPublications[etag] = sp;
         sp->dispatch(request);
      }
      else
      {
         // per 3903 (sec 6.5), a PUB w/ no SIPIfMatch must have contents. .mjf.
         auto response = std::make_shared<SipMessage>();
         makeResponse(*response, request, 400);
         send(response);
      }
   }
}

bool
DialogUsageManager::checkEventPackage(const SipMessage& request)
{
   int failureCode = 0;
   MethodTypes method = request.header(h_RequestLine).method();

//       || (method == NOTIFY && !request.exists(h_SubscriptionState)))

   if (!request.exists(h_Event))
   {
      InfoLog (<< "No Event header in " << request.header(h_RequestLine).unknownMethodName());
      failureCode = 400;
   }
   else
   {
      switch(method)
      {
         case SUBSCRIBE:
            if (!getServerSubscriptionHandler(request.header(h_Event).value()))
            {
               InfoLog (<< "No handler for event package for SUBSCRIBE: " 
                        << request.header(h_Event).value());
               failureCode = 489;
            }
            break;
         case NOTIFY:
            if (!getClientSubscriptionHandler(request.header(h_Event).value()))
            {
               InfoLog (<< "No handler for event package for NOTIFY: " 
                        << request.header(h_Event).value());
               failureCode = 489;
            }
            break;
         case PUBLISH:
            if (!getServerPublicationHandler(request.header(h_Event).value()))
            {
               InfoLog (<< "No handler for event package for PUBLISH: " 
                        << request.header(h_Event).value());
               failureCode = 489;
            }
            break;
         default:
            resip_assert(0);
      }
   }

   if (failureCode > 0)
   {
      auto response = std::make_shared<SipMessage>();
      makeResponse(*response, request, failureCode);
      if(failureCode == 489)
      {
         response->header(h_AllowEvents) = getMasterProfile()->getAllowedEvents();
      }
      send(response);
      return false;
   }
   return true;
}

DialogSet*
DialogUsageManager::findDialogSet(const DialogSetId& id)
{
   threadCheck();
   StackLog ( << "Looking for dialogSet: " << id << " in map:");
   StackLog ( << "DialogSetMap: " << InserterP(mDialogSetMap) );
   DialogSetMap::const_iterator it = mDialogSetMap.find(id);

   if (it == mDialogSetMap.end())
   {
      StackLog ( << "Not found" );
      return 0;
   }
   else
   {
      if(it->second->isDestroying())
      {
         StackLog ( << "isDestroying() == true" );
         return 0;
      }
      else
      {
         StackLog ( << "found" );
         return it->second;
      }
   }
}

BaseCreator*
DialogUsageManager::findCreator(const DialogId& id)
{
   DialogSet* ds = findDialogSet(id.getDialogSetId());
   if (ds)
   {
      return ds->getCreator();
   }
   else
   {
      return 0;
   }
}

void
DialogUsageManager::removeDialogSet(const DialogSetId& dsId)
{
   StackLog ( << "************* Removing DialogSet ***************: " << dsId);
   //StackLog ( << "Before: " << Inserter(mDialogSetMap) );
   mDialogSetMap.erase(dsId);
   StackLog ( << "DialogSetMap: " << InserterP(mDialogSetMap) );
   if (mRedirectManager)
   {
      mRedirectManager->removeDialogSet(dsId);
   }
}

ClientSubscriptionHandler*
DialogUsageManager::getClientSubscriptionHandler(const Data& eventType)
{
   map<Data, ClientSubscriptionHandler*>::iterator res = mClientSubscriptionHandlers.find(eventType);
   if (res != mClientSubscriptionHandlers.end())
   {
      return res->second;
   }
   else
   {
      return 0;
   }
}

ServerSubscriptionHandler*
DialogUsageManager::getServerSubscriptionHandler(const Data& eventType)
{
   map<Data, ServerSubscriptionHandler*>::iterator res = mServerSubscriptionHandlers.find(eventType);
   if (res != mServerSubscriptionHandlers.end())
   {
      return res->second;
   }
   else
   {
      return 0;
   }
}

ClientPublicationHandler*
DialogUsageManager::getClientPublicationHandler(const Data& eventType)
{
   map<Data, ClientPublicationHandler*>::iterator res = mClientPublicationHandlers.find(eventType);
   if (res != mClientPublicationHandlers.end())
   {
      return res->second;
   }
   else
   {
      return 0;
   }
}

ServerPublicationHandler*
DialogUsageManager::getServerPublicationHandler(const Data& eventType)
{
   map<Data, ServerPublicationHandler*>::iterator res = mServerPublicationHandlers.find(eventType);
   if (res != mServerPublicationHandlers.end())
   {
      return res->second;
   }
   else
   {
      return 0;
   }
}

OutOfDialogHandler*
DialogUsageManager::getOutOfDialogHandler(const MethodTypes type)
{
   map<MethodTypes, OutOfDialogHandler*>::iterator res = mOutOfDialogHandlers.find(type);
   if (res != mOutOfDialogHandlers.end())
   {
      return res->second;
   }
   else
   {
      return 0;
   }
}

void 
DialogUsageManager::addIncomingFeature(std::shared_ptr<DumFeature> feat)
{
   mIncomingFeatureList.emplace_back(feat);
}

void
DialogUsageManager::addOutgoingFeature(std::shared_ptr<DumFeature> feat)
{
   // make sure EncryptionManager is the last feature in the list.
   mOutgoingFeatureList.emplace(std::begin(mOutgoingFeatureList), feat);
}

void
DialogUsageManager::setOutgoingMessageInterceptor(std::shared_ptr<DumFeature> feat) noexcept
{
   mOutgoingMessageInterceptor = feat;
}

void
DialogUsageManager::applyToAllServerSubscriptions(ServerSubscriptionFunctor* functor)
{
   resip_assert(functor);
   for (DialogSetMap::iterator it = mDialogSetMap.begin(); it != mDialogSetMap.end(); ++it)
   {
      for (DialogSet::DialogMap::iterator i = it->second->mDialogs.begin(); i != it->second->mDialogs.end(); ++i)
      {
         std::vector<ServerSubscriptionHandle> serverSubs = i->second->getServerSubscriptions();
         for (std::vector<ServerSubscriptionHandle>::iterator iss = serverSubs.begin(); iss != serverSubs.end(); ++iss)
         {
            functor->apply(*iss);
         }
      }
   }
}

void
DialogUsageManager::applyToAllClientSubscriptions(ClientSubscriptionFunctor* functor)
{
   resip_assert(functor);
   for (DialogSetMap::iterator it = mDialogSetMap.begin(); it != mDialogSetMap.end(); ++it)
   {
      for (DialogSet::DialogMap::iterator i = it->second->mDialogs.begin(); i != it->second->mDialogs.end(); ++i)
      {
         std::vector<ClientSubscriptionHandle> clientSubs = i->second->getClientSubscriptions();
         for (std::vector<ClientSubscriptionHandle>::iterator ics = clientSubs.begin(); ics != clientSubs.end(); ++ics)
         {
            functor->apply(*ics);
         }
      }
   }
}

void 
DialogUsageManager::endAllServerSubscriptions(TerminateReason reason)
{
   // Make a copy of the map - since calling end can cause an immediate delete this on the subscription and thus cause
   // the object to remove itself from the mServerSubscriptions map, messing up our iterator
   ServerSubscriptions tempSubscriptions = mServerSubscriptions;
   ServerSubscriptions::iterator it = tempSubscriptions.begin();
   for (; it != tempSubscriptions.end(); it++)
   {
      it->second->end(reason);
   }
}

void 
DialogUsageManager::endAllServerPublications()
{
   // Make a copy of the map - since calling end can cause an immediate delete this on the publication and thus cause
   // the object to remove itself from the mServerPublications map, messing up our iterator
   ServerPublications tempPublications = mServerPublications;
   ServerPublications::iterator it = tempPublications.begin();
   for (; it != tempPublications.end(); it++)
   {
      it->second->end();
   }
}

void
DialogUsageManager::registerForConnectionTermination(Postable* listener)
{
   mConnectionTerminatedEventDispatcher.addListener(listener);
}

void
DialogUsageManager::unRegisterForConnectionTermination(Postable* listener)
{
   mConnectionTerminatedEventDispatcher.removeListener(listener);
}

void
DialogUsageManager::requestMergedRequestRemoval(const MergedRequestKey& key)
{
   // Only post delayed merge request removal if running, if we are shutting down, then there is no need
   if (mShutdownState == Running)
   {
       DebugLog(<< "Got merged request removal request");
       MergedRequestRemovalCommand command(*this, key);
       mStack.postMS(command, Timer::TF, this);
   }
}

void
DialogUsageManager::removeMergedRequest(const MergedRequestKey& key)
{
   DebugLog(<< "Merged request removed");
   mMergedRequests.erase(key);
}

TargetCommand::Target& 
DialogUsageManager::dumIncomingTarget() 
{
   return *mIncomingTarget;
}

TargetCommand::Target& 
DialogUsageManager::dumOutgoingTarget() 
{
   return *mOutgoingTarget;
}

DialogEventStateManager* 
DialogUsageManager::createDialogEventStateManager(DialogEventHandler* handler)
{
   if(handler)
   {
      mDialogEventStateManager = new DialogEventStateManager();
      mDialogEventStateManager->mDialogEventHandler = handler;
   }
   else
   {
      delete mDialogEventStateManager;
      mDialogEventStateManager=0;
   }
   return mDialogEventStateManager;
}

void 
DialogUsageManager::setAdvertisedCapabilities(SipMessage& msg, const std::shared_ptr<UserProfile>& userProfile)
{
   if(userProfile->isAdvertisedCapability(Headers::Allow)) 
   {
      msg.header(h_Allows) = getMasterProfile()->getAllowedMethods();
   }
   if(userProfile->isAdvertisedCapability(Headers::AcceptEncoding)) 
   {
      msg.header(h_AcceptEncodings) = getMasterProfile()->getSupportedEncodings();
   }
   if(userProfile->isAdvertisedCapability(Headers::AcceptLanguage)) 
   {
      msg.header(h_AcceptLanguages) = getMasterProfile()->getSupportedLanguages();
   }
   if(userProfile->isAdvertisedCapability(Headers::AllowEvents)) 
   {
      msg.header(h_AllowEvents) = getMasterProfile()->getAllowedEvents();
   }
   if(userProfile->isAdvertisedCapability(Headers::Supported)) 
   {
      msg.header(h_Supporteds) = getMasterProfile()->getSupportedOptionTags();
   }
}

/* ====================================================================
 * The Vovida Software License, Version 1.0
 *
 * Copyright (c) 2000 Vovida Networks, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The names "VOCAL", "Vovida Open Communication Application Library",
 *    and "Vovida Open Communication Application Library (VOCAL)" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact vocal@vovida.org.
 *
 * 4. Products derived from this software may not be called "VOCAL", nor
 *    may "VOCAL" appear in their name, without prior written
 *    permission of Vovida Networks, Inc.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL VOVIDA
 * NETWORKS, INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT DAMAGES
 * IN EXCESS OF $1,000, NOR FOR ANY INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * ====================================================================
 *
 * This software consists of voluntary contributions made by Vovida
 * Networks, Inc. and many individuals on behalf of Vovida Networks,
 * Inc.  For more information on Vovida Networks, Inc., please see
 * <http://www.vovida.org/>.
 *
 */
