////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "UpgradeFeature.h"

#include "Cluster/ClusterFeature.h"
#include "Logger/Logger.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/InitDatabaseFeature.h"
#include "V8/v8-conv.h"
#include "V8/v8-globals.h"
#include "V8Server/V8Context.h"
#include "V8Server/V8DealerFeature.h"
#include "V8Server/v8-vocbase.h"
#include "VocBase/vocbase.h"

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::basics;
using namespace arangodb::options;

UpgradeFeature::UpgradeFeature(
    ApplicationServer* server, int* result,
    std::vector<std::string> const& nonServerFeatures)
    : ApplicationFeature(server, "Upgrade"),
      _upgrade(false),
      _upgradeCheck(true),
      _result(result),
      _nonServerFeatures(nonServerFeatures) {
  setOptional(false);
  requiresElevatedPrivileges(false);
  startsAfter("CheckVersion");
  startsAfter("Cluster");
  startsAfter("Database");
  startsAfter("MMFilesWalRecovery");
  startsAfter("V8Dealer");
  startsAfter("Aql");
}

void UpgradeFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  options->addSection("database", "Configure the database");

  options->addOldOption("upgrade", "database.auto-upgrade");

  options->addOption("--database.auto-upgrade",
                     "perform a database upgrade if necessary",
                     new BooleanParameter(&_upgrade));

  options->addHiddenOption("--database.upgrade-check",
                           "skip a database upgrade",
                           new BooleanParameter(&_upgradeCheck));
}

void UpgradeFeature::validateOptions(std::shared_ptr<ProgramOptions> options) {
  if (_upgrade && !_upgradeCheck) {
    LOG_TOPIC(FATAL, arangodb::Logger::FIXME) << "cannot specify both '--database.auto-upgrade true' and "
                  "'--database.upgrade-check false'";
    FATAL_ERROR_EXIT();
  }

  if (!_upgrade) {
    LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "executing upgrade check: not disabling server features";
    return;
  }

  LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "executing upgrade procedure: disabling server features";

  ApplicationServer::forceDisableFeatures(_nonServerFeatures);

  DatabaseFeature* database =
      ApplicationServer::getFeature<DatabaseFeature>("Database");
  database->disableReplicationApplier();
  database->enableUpgrade();

  ClusterFeature* cluster =
      ApplicationServer::getFeature<ClusterFeature>("Cluster");
  cluster->forceDisable();
}

void UpgradeFeature::start() {
  auto init =
      ApplicationServer::getFeature<InitDatabaseFeature>("InitDatabase");

  // upgrade the database
  if (_upgradeCheck) {
    upgradeDatabase(init->defaultPassword());
  }

  // change admin user
  if (init->restoreAdmin()) {
    changeAdminPassword(init->defaultPassword());
  }

  // and force shutdown
  if (_upgrade || init->isInitDatabase() || init->restoreAdmin()) {
    if (init->isInitDatabase()) {
      *_result = EXIT_SUCCESS;
    }
    
    server()->beginShutdown();
  }
}

void UpgradeFeature::changeAdminPassword(std::string const& defaultPassword) {
  LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "starting to restore admin user";

  auto* systemVocbase = DatabaseFeature::DATABASE->systemDatabase();

  // enter context and isolate
  {
    V8Context* context =
        V8DealerFeature::DEALER->enterContext(systemVocbase, true, 0);

    if (context == nullptr) {
      LOG_TOPIC(FATAL, arangodb::Logger::FIXME) << "could not enter context #0";
      FATAL_ERROR_EXIT();
    }

    TRI_DEFER(V8DealerFeature::DEALER->exitContext(context));

    {
      v8::HandleScope scope(context->_isolate);
      auto localContext =
          v8::Local<v8::Context>::New(context->_isolate, context->_context);
      localContext->Enter();

      {
        v8::Context::Scope contextScope(localContext);

        // run upgrade script
        LOG_TOPIC(DEBUG, arangodb::Logger::FIXME) << "running admin recreation script";

        // special check script to be run just once in first thread (not in
        // all) but for all databases
        v8::HandleScope scope(context->_isolate);

        v8::Handle<v8::Object> args = v8::Object::New(context->_isolate);

        args->Set(TRI_V8_ASCII_STRING2(context->_isolate, "password"),
                  TRI_V8_STD_STRING2(context->_isolate, defaultPassword));

        localContext->Global()->Set(
            TRI_V8_ASCII_STRING2(context->_isolate, "UPGRADE_ARGS"), args);

        auto startupLoader = V8DealerFeature::DEALER->startupLoader();

        startupLoader->executeGlobalScript(context->_isolate, localContext,
                                           "server/restore-admin-user.js");
      }

      // finally leave the context. otherwise v8 will crash with assertion
      // failure when we delete the context locker below
      localContext->Exit();
    }
  }

  // and return from the context
  LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "finished to restore admin user";

  *_result = EXIT_SUCCESS;
}

void UpgradeFeature::upgradeDatabase(std::string const& defaultPassword) {
  LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "starting database init/upgrade";

  DatabaseFeature* databaseFeature = application_features::ApplicationServer::getFeature<DatabaseFeature>("Database");
  auto* systemVocbase = DatabaseFeature::DATABASE->systemDatabase();

  // enter context and isolate
  {
    V8Context* context =
        V8DealerFeature::DEALER->enterContext(systemVocbase, true, 0);

    if (context == nullptr) {
      LOG_TOPIC(FATAL, arangodb::Logger::FIXME) << "could not enter context #0";
      FATAL_ERROR_EXIT();
    }

    TRI_DEFER(V8DealerFeature::DEALER->exitContext(context));

    {
      v8::HandleScope scope(context->_isolate);
      auto localContext =
          v8::Local<v8::Context>::New(context->_isolate, context->_context);
      localContext->Enter();

      {
        v8::Context::Scope contextScope(localContext);

        // run upgrade script
        LOG_TOPIC(DEBUG, arangodb::Logger::FIXME) << "running database init/upgrade";

        for (auto& name : databaseFeature->getDatabaseNames()) {
          TRI_vocbase_t* vocbase = databaseFeature->lookupDatabase(name);
          TRI_ASSERT(vocbase != nullptr);

          // special check script to be run just once in first thread (not in
          // all) but for all databases
          v8::HandleScope scope(context->_isolate);

          v8::Handle<v8::Object> args = v8::Object::New(context->_isolate);

          args->Set(TRI_V8_ASCII_STRING2(context->_isolate, "upgrade"),
                    v8::Boolean::New(context->_isolate, _upgrade));

          args->Set(TRI_V8_ASCII_STRING2(context->_isolate, "password"),
                    TRI_V8_STD_STRING2(context->_isolate, defaultPassword));

          localContext->Global()->Set(
              TRI_V8_ASCII_STRING2(context->_isolate, "UPGRADE_ARGS"), args);

          bool ok = TRI_UpgradeDatabase(vocbase, localContext);

          if (!ok) {
            if (localContext->Global()->Has(TRI_V8_ASCII_STRING2(
                    context->_isolate, "UPGRADE_STARTED"))) {
            
              uint64_t upgradeType = TRI_ObjectToUInt64(localContext->Global()->Get(TRI_V8_ASCII_STRING2(context->_isolate, "UPGRADE_TYPE")), false);

              localContext->Exit();
  // 0 = undecided
  // 1 = same version
  // 2 = downgrade
  // 3 = upgrade
  // 4 = requires upgrade
  // 5 = no version found
              char const* typeName = "initialization";

              switch (upgradeType) {
                case 0: // undecided
                case 1: // same version
                case 2: // downgrade
                case 5: // no VERSION file found
                  // initialization
                  break;
                case 3: // upgrade
                  typeName = "upgrade";
                  break;
                case 4: // requires upgrade
                  typeName = "upgrade";
                  if (!_upgrade) {
                    LOG_TOPIC(FATAL, arangodb::Logger::FIXME)
                        << "Database '" << vocbase->name()
                        << "' needs upgrade. Please start the server with the "
                           "--database.auto-upgrade option";
                  }
                  break;
              }

              LOG_TOPIC(FATAL, arangodb::Logger::FIXME) << "Database '" << vocbase->name()
                         << "' " << typeName << " failed. Please inspect the logs from "
                            "the " << typeName << " procedure";
            } else {
              LOG_TOPIC(FATAL, arangodb::Logger::FIXME) << "JavaScript error during server start";
            }
              
            FATAL_ERROR_EXIT();
          }
            
          LOG_TOPIC(DEBUG, arangodb::Logger::FIXME) << "database '" << vocbase->name()
                     << "' init/upgrade done";
        }
      }

      // finally leave the context. otherwise v8 will crash with assertion
      // failure when we delete the context locker below
      localContext->Exit();
    }
  }

  if (_upgrade) {
    *_result = EXIT_SUCCESS;
    LOG_TOPIC(INFO, arangodb::Logger::FIXME) << "database upgrade passed";
  }

  // and return from the context
  LOG_TOPIC(TRACE, arangodb::Logger::FIXME) << "finished database init/upgrade";
}
