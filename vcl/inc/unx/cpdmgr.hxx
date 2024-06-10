/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#pragma once

#include <config_dbus.h>
#include <config_gio.h>

#if ENABLE_DBUS && ENABLE_GIO
#include <gio/gio.h>
#else
typedef struct _GDBusProxy GDBusProxy;
typedef struct _GDBusConnection GDBusConnection;
#endif

#include <printerinfomanager.hxx>
#include "cupsmgr.hxx"
#include <cpdb/frontend.h>

#define BACKEND_DIR "/usr/share/print-backends"
#define BACKEND_INTERFACE "/usr/share/dbus-1/interfaces/org.openprinting.Backend.xml"

namespace psp
{
class PPDParser;

// struct CPDPrinter
// {
//     const char* id;
//     const char* name;
//     const char* info;
//     const char* location;
//     const char* make_and_model;
//     const char* printer_state;
//     const char* backend_name;
//     bool is_accepting_jobs;
//     GDBusProxy* backend;
// };

class CPDManager final : public PrinterInfoManager
{
#if ENABLE_DBUS && ENABLE_GIO
    bool m_aPrintersChanged = true;
    std::unordered_map<FILE*, OString, FPtrHash> m_aSpoolFiles;
    std::unordered_map<OUString, cpdb_printer_obj_t*> m_aCPDDestMap;
    std::unordered_map<OUString, PPDContext> m_aDefaultContexts;
    cpdb_frontend_obj_t *frontendObj;
#endif
    CPDManager();
    // Function called when CPDManager is destroyed
    virtual ~CPDManager() override;

    virtual void initialize() override;

#if ENABLE_DBUS && ENABLE_GIO
    static void printerUpdateCallback(cpdb_frontend_obj_t* frontendObj,
                          cpdb_printer_obj_t* pDest,
                          cpdb_printer_update_t change);
    static void printerStateChanged(GDBusConnection* connection, const gchar* sender_name,
                              const gchar* object_path, const gchar* interface_name, 
                              const gchar* signal_name, GVariant* parameters, gpointer user_data);
    static void fillBasicOptions(cpdb_printer_obj_t *pDest,
                          GVariant *parameters);
    static void printerAdded(GDBusConnection* connection, const gchar* sender_name,
                             const gchar* object_path, const gchar* interface_name,
                             const gchar* signal_name, GVariant* parameters, gpointer user_data);
    static void printerRemoved(GDBusConnection* connection, const gchar* sender_name,
                               const gchar* object_path, const gchar* interface_name,
                               const gchar* signal_name, GVariant* parameters, gpointer user_data);

    static cpdb_settings_t *getOptionsFromDocumentSetup(const JobData& rJob, bool bBanner,
                                            const OString& rJobName, int& rNumOptions);
    static void updatePrinters(gpointer key, gpointer value, gpointer user_data);
    static void addPrinters(gpointer key, gpointer value, gpointer user_data);
#endif

public:
#if ENABLE_DBUS && ENABLE_GIO
    // Functions involved in initialization
    void addNewPrinter(const OUString&, const OUString&, cpdb_printer_obj_t*);

#endif

    // Create CPDManager
    static CPDManager* tryLoadCPD();

    // Create a PPDParser for CPD Printers
    const PPDParser* createCPDParser(const OUString& rPrinter);

    // Functions related to printing
    virtual FILE* startSpool(const OUString& rPrinterName, bool bQuickCommand) override;
    virtual bool endSpool(const OUString& rPrinterName, const OUString& rJobTitle, FILE* pFile,
                          const JobData& rDocumentJobData, bool bBanner,
                          const OUString& rFaxNumber) override;
    virtual void setupJobContextData(JobData& rData) override;

    // check if the printer configuration has changed
    virtual bool checkPrintersChanged(bool) override;
};

} // namespace psp

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
