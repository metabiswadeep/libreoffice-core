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

#include <sal/config.h>

#include <cstddef>
#include <unistd.h>

#include <unx/cpdmgr.hxx>

#include <osl/file.h>
#include <osl/thread.h>

#include <rtl/ustrbuf.hxx>
#include <sal/log.hxx>

#include <config_dbus.h>
#include <config_gio.h>

#include <cpdb/cpdb-frontend.h>

using namespace psp;

#if ENABLE_DBUS && ENABLE_GIO
// Function to execute when name is acquired on the bus
void CPDManager::onNameAcquired(GDBusConnection* connection, const gchar*, gpointer user_data)
{
    GError *error = NULL;
    cpdb_frontend_obj_t *f = static_cast<cpdb_frontend_obj_t *> (user_data);
    
    g_dbus_connection_signal_subscribe(connection,
                                       NULL,                            //Sender name
                                       "org.openprinting.PrintBackend", //Sender interface
                                       CPDB_SIGNAL_PRINTER_ADDED,       //Signal name
                                       NULL,                            /**match on all object paths**/
                                       NULL,                            /**match on all arguments**/
                                       0,                               //Flags
                                       on_printer_added,                //callback
                                       user_data,                       //user_data
                                       NULL);

    g_dbus_connection_signal_subscribe(connection,
                                       NULL,                            //Sender name
                                       "org.openprinting.PrintBackend", //Sender interface
                                       CPDB_SIGNAL_PRINTER_REMOVED,     //Signal name
                                       NULL,                            /**match on all object paths**/
                                       NULL,                            /**match on all arguments**/
                                       0,                               //Flags
                                       on_printer_removed,              //callback
                                       user_data,                       //user_data
                                       NULL);
    g_dbus_connection_signal_subscribe(connection,
                                       NULL,                                //Sender name
                                       "org.openprinting.PrintBackend",     //Sender interface
                                       CPDB_SIGNAL_PRINTER_STATE_CHANGED,   //Signal name
                                       NULL,                                /**match on all object paths**/
                                       NULL,                                /**match on all arguments**/
                                       0,                                   //Flags
                                       on_printer_state_changed,            //callback
                                       user_data,                           //user_data
                                       NULL);

    g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(f->skeleton),
                                     connection, 
                                     CPDB_DIALOG_OBJ_PATH,
                                     &error);
    if (error)
    {
        g_message("Error exporting frontend interface : %s\n", error->message);
        return;
    }
    
    cpdbActivateBackends(f);
    f->name_done = TRUE;
}

void CPDManager::onNameLost(GDBusConnection* connection, const gchar* name, gpointer user_data)
{
    g_message("Name Lost: %s", name);
    cpdb_frontend_obj_t *f = user_data;
    f->name_done = TRUE;
}

void CPDManager::onStateChanged(GDBusConnection* connection, const gchar* sender_name,
                              const gchar* object_path, const gchar* interface_name, const gchar* signal_name,
                              GVariant* parameters, gpointer user_data){
    //Implement state changed signal
    cpdb_frontend_obj_t *f = static_cast<cpdb_frontend_obj_t *> (user_data);
    gboolean printer_is_accepting_jobs;
    char *printer_id, *printer_state, *backend_name;

    g_variant_get(parameters, "(ssbs)", &printer_id, &printer_state,
                    &printer_is_accepting_jobs, &backend_name);
    cpdb_printer_obj_t *p = cpdbFindPrinterObj(f, printer_id, backend_name);
    if (p->state)
        free(p->state);
    p->state = cpdbGetStringCopy(printer_state);
    p->accepting_jobs = printer_is_accepting_jobs;
    f->printer_cb(f, p, CPDB_CHANGE_PRINTER_STATE_CHANGED);
}

void CPDManager::printerAdded(GDBusConnection* connection, const gchar* sender_name,
                              const gchar* object_path, const gchar* interface_name, const gchar* signal_name,
                              GVariant* parameters, gpointer user_data)
{
    cpdb_frontend_obj_t *f = static_cast<cpdb_frontend_obj_t *> (user_data);
    cpdb_printer_obj_t *p = cpdbGetNewPrinterObj();
    
    if (f->last_saved_settings != NULL)
    {
        cpdbCopySettings(f->last_saved_settings, p->settings);
    }
    cpdbFillBasicOptions(p, parameters);
    cpdbAddPrinter(f, p);
    f->printer_cb(f, p, CPDB_CHANGE_PRINTER_ADDED);
}

void CPDManager::printerRemoved(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                const gchar*, GVariant* parameters, gpointer user_data)
{
    cpdb_frontend_obj_t *f = static_cast<cpdb_frontend_obj_t *> (user_data);
    char *printer_id;
    char *backend_name;
    
    g_variant_get(parameters, "(ss)", &printer_id, &backend_name);
    cpdb_printer_obj_t *p = cpdbRemovePrinter(f, printer_id, backend_name);
    f->printer_cb(f, p, CPDB_CHANGE_PRINTER_REMOVED);
}

GDBusProxy* CPDManager::getProxy(const std::string& target)
{
    std::unordered_map<std::string, GDBusProxy*>::const_iterator it = m_pBackends.find(target);
    if (it == m_pBackends.end())
    {
        return nullptr;
    }
    return it->second;
}

void CPDManager::addBackend(std::pair<std::string, GDBusProxy*> pair) { m_pBackends.insert(pair); }

void CPDManager::addTempBackend(const std::pair<std::string, gchar*>& pair)
{
    m_tBackends.push_back(pair);
}

std::vector<std::pair<std::string, gchar*>> const& CPDManager::getTempBackends() const
{
    return m_tBackends;
}

void CPDManager::addNewPrinter(const OUString& aPrinterName, const OUString& aUniqueName,
                               CPDPrinter* pDest)
{
    m_aCPDDestMap[aUniqueName] = pDest;
    bool bSetToGlobalDefaults = m_aPrinters.find(aUniqueName) == m_aPrinters.end();
    Printer aPrinter = m_aPrinters[aUniqueName];
    if (bSetToGlobalDefaults)
        aPrinter.m_aInfo = m_aGlobalDefaults;
    aPrinter.m_aInfo.m_aPrinterName = aPrinterName;

    // TODO: I don't know how this should work when we have multiple
    // sources with multiple possible defaults for each
    // if( pDest->is_default )
    //     m_aDefaultPrinter = aPrinterName;

    rtl_TextEncoding aEncoding = osl_getThreadTextEncoding();
    aPrinter.m_aInfo.m_aComment = OStringToOUString(pDest->info, aEncoding);
    aPrinter.m_aInfo.m_aLocation = OStringToOUString(pDest->location, aEncoding);
    // note: the parser that goes with the PrinterInfo
    // is created implicitly by the JobData::operator=()
    // when it detects the NULL ptr m_pParser.
    // if we wanted to fill in the parser here this
    // would mean we'd have to send a dbus message for each and
    // every printer - which would be really bad runtime
    // behaviour
    aPrinter.m_aInfo.m_pParser = nullptr;
    aPrinter.m_aInfo.m_aContext.setParser(nullptr);
    std::unordered_map<OUString, PPDContext>::const_iterator c_it
        = m_aDefaultContexts.find(aUniqueName);
    if (c_it != m_aDefaultContexts.end())
    {
        aPrinter.m_aInfo.m_pParser = c_it->second.getParser();
        aPrinter.m_aInfo.m_aContext = c_it->second;
    }
    aPrinter.m_aInfo.m_aDriverName = "CPD:" + aUniqueName;
    m_aPrinters[aUniqueName] = aPrinter;
}
#endif

/*
 *  CPDManager class
 */

CPDManager* CPDManager::tryLoadCPD()
{
    CPDManager* pManager = nullptr;
#if ENABLE_DBUS && ENABLE_GIO
    static const char* pEnv = getenv("SAL_DISABLE_CPD");

    if (!pEnv || !*pEnv)
    {
        // interface description XML files are needed in 'onNameAcquired()'
        if (!g_file_test(BACKEND_INTERFACE, G_FILE_TEST_IS_REGULAR))
        {
            return nullptr;
        }

        GDir* dir;
        const gchar* filename;
        dir = g_dir_open(BACKEND_DIR, 0, nullptr);
        if (dir != nullptr)
        {
            while ((filename = g_dir_read_name(dir)))
            {
                if (pManager == nullptr)
                {
                    pManager = new CPDManager();
                }
                gchar* contents;
                std::stringstream filepath;
                filepath << BACKEND_DIR << '/' << filename;
                if (g_file_get_contents(filepath.str().c_str(), &contents, nullptr, nullptr))
                {
                    std::pair<std::string, gchar*> new_tbackend(filename, contents);
                    pManager->addTempBackend(new_tbackend);
                    g_free(contents);
                }
            }
            g_dir_close(dir);
        }
    }
#endif
    return pManager;
}

CPDManager::CPDManager()
    : PrinterInfoManager(PrinterInfoManager::Type::CPD)
{
#if ENABLE_DBUS && ENABLE_GIO
    // Get Destinations number and pointers
    GError* error = nullptr;
    m_pConnection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    g_assert_no_error(error);
#endif
}

CPDManager::~CPDManager()
{
#if ENABLE_DBUS && ENABLE_GIO
    g_dbus_connection_emit_signal(m_pConnection, nullptr, "/org/libreoffice/PrintDialog",
                                  "org.openprinting.PrintFrontend", "StopListing", nullptr,
                                  nullptr);
    g_dbus_connection_flush_sync(m_pConnection, nullptr, nullptr);
    g_dbus_connection_close_sync(m_pConnection, nullptr, nullptr);
    for (auto const& backend : m_pBackends)
    {
        g_object_unref(backend.second);
    }
    for (auto const& backend : m_aCPDDestMap)
    {
        free(backend.second);
    }
#endif
}

const PPDParser* CPDManager::createCPDParser(const OUString& rPrinter)
{
    const PPDParser* pNewParser = nullptr;
#if ENABLE_DBUS && ENABLE_GIO
    OUString aPrinter;

    if (rPrinter.startsWith("CPD:"))
        aPrinter = rPrinter.copy(4);
    else
        aPrinter = rPrinter;

    std::unordered_map<OUString, CPDPrinter*>::iterator dest_it = m_aCPDDestMap.find(aPrinter);

    if (dest_it != m_aCPDDestMap.end())
    {
        CPDPrinter* pDest = dest_it->second;
        GVariant* ret = nullptr;
        GError* error = nullptr;
        ret = g_dbus_proxy_call_sync(pDest->backend, "GetAllOptions",
                                     g_variant_new("(s)", (pDest->id)), G_DBUS_CALL_FLAGS_NONE, -1,
                                     nullptr, &error);
        if (ret != nullptr && error == nullptr)
        {
            // TODO: These keys need to be redefined to preserve usage across libreoffice
            // InputSlot - media-col.media-source?
            // Font - not needed now as it is required only for ps and we are using pdf
            // Dial? - for FAX (need to look up PWG spec)

            int num_attribute;
            GVariantIter *iter_attr, *iter_supported_values;
            g_variant_get(ret, "(ia(ssia(s)))", &num_attribute, &iter_attr);
            rtl_TextEncoding aEncoding = osl_getThreadTextEncoding();
            PPDKey* pKey = nullptr;
            OUString aValueName;
            PPDValue* pValue;
            std::vector<PPDKey*> keys;
            std::vector<OUString> default_values;
            for (int i = 0; i < num_attribute; i++)
            {
                char *name, *default_value;
                int num_supported_values;
                g_variant_iter_loop(iter_attr, "(ssia(s))", &name, &default_value,
                                    &num_supported_values, &iter_supported_values);
                OUString aOptionName = OStringToOUString(name, aEncoding);
                OUString aDefaultValue = OStringToOUString(default_value, aEncoding);
                if (aOptionName == "sides")
                {
                    // Duplex key is used throughout for checking Duplex Support
                    aOptionName = u"Duplex"_ustr;
                }
                else if (aOptionName == "printer-resolution")
                {
                    // Resolution key is used in places
                    aOptionName = u"Resolution"_ustr;
                }
                else if (aOptionName == "media")
                {
                    // PageSize key is used in many places
                    aOptionName = u"PageSize"_ustr;
                }
                default_values.push_back(aDefaultValue);
                pKey = new PPDKey(aOptionName);

                // If number of values are 0, this is not settable via UI
                if (num_supported_values > 0 && aDefaultValue != "NA")
                    pKey->m_bUIOption = true;

                bool bDefaultFound = false;

                for (int j = 0; j < num_supported_values; j++)
                {
                    char* value;
                    g_variant_iter_loop(iter_supported_values, "(s)", &value);
                    aValueName = OStringToOUString(value, aEncoding);
                    if (aOptionName == "Duplex")
                    {
                        // Duplex key matches against very specific Values
                        if (aValueName == "one-sided")
                        {
                            aValueName = u"None"_ustr;
                        }
                        else if (aValueName == "two-sided-long-edge")
                        {
                            aValueName = u"DuplexNoTumble"_ustr;
                        }
                        else if (aValueName == "two-sided-short-edge")
                        {
                            aValueName = u"DuplexTumble"_ustr;
                        }
                    }

                    pValue = pKey->insertValue(aValueName, eQuoted);
                    if (!pValue)
                        continue;
                    pValue->m_aValue = aValueName;

                    if (aValueName.equals(aDefaultValue))
                    {
                        pKey->m_pDefaultValue = pValue;
                        bDefaultFound = true;
                    }
                }
                // This could be done to ensure default values also appear as options:
                if (!bDefaultFound && pKey->m_bUIOption)
                {
                    //     pValue = pKey->insertValue( aDefaultValue, eQuoted );
                    //     if( pValue )
                    //         pValue->m_aValue = aDefaultValue;
                }
                keys.emplace_back(pKey);
            }

            pKey = new PPDKey(u"ModelName"_ustr);
            aValueName = OStringToOUString("", aEncoding);
            pValue = pKey->insertValue(aValueName, eQuoted);
            if (pValue)
                pValue->m_aValue = aValueName;
            pKey->m_pDefaultValue = pValue;
            keys.emplace_back(pKey);

            pKey = new PPDKey(u"NickName"_ustr);
            aValueName = OStringToOUString(pDest->name, aEncoding);
            pValue = pKey->insertValue(aValueName, eQuoted);
            if (pValue)
                pValue->m_aValue = aValueName;
            pKey->m_pDefaultValue = pValue;
            keys.emplace_back(pKey);

            pNewParser = new PPDParser(aPrinter, keys);
            PrinterInfo& rInfo = m_aPrinters[aPrinter].m_aInfo;
            PPDContext& rContext = m_aDefaultContexts[aPrinter];
            rContext.setParser(pNewParser);
            setDefaultPaper(rContext);
            std::vector<OUString>::iterator defit = default_values.begin();
            for (auto const& key : keys)
            {
                const PPDValue* p1Value = key->getValue(*defit);
                if (p1Value)
                {
                    if (p1Value != key->getDefaultValue())
                    {
                        rContext.setValue(key, p1Value, true);
                        SAL_INFO("vcl.unx.print",
                                 "key " << pKey->getKey() << " is set to " << *defit);
                    }
                    else
                        SAL_INFO("vcl.unx.print",
                                 "key " << pKey->getKey() << " is defaulted to " << *defit);
                }
                ++defit;
            }

            rInfo.m_pParser = pNewParser;
            rInfo.m_aContext = rContext;
            g_variant_unref(ret);
        }
        else
        {
            g_clear_error(&error);
            SAL_INFO("vcl.unx.print", "CPD GetAllOptions failed, falling back to generic driver");
        }
    }
    else
        SAL_INFO("vcl.unx.print", "no dest found for printer " << aPrinter);

    if (!pNewParser)
    {
        // get the default PPD
        pNewParser = PPDParser::getParser(u"SGENPRT"_ustr);
        SAL_WARN("vcl.unx.print", "Parsing default SGENPRT PPD");

        PrinterInfo& rInfo = m_aPrinters[aPrinter].m_aInfo;

        rInfo.m_pParser = pNewParser;
        rInfo.m_aContext.setParser(pNewParser);
    }
#else
    (void)rPrinter;
#endif
    return pNewParser;
}

void CPDManager::initialize()
{
    // get normal printers, clear printer list
    PrinterInfoManager::initialize();
#if ENABLE_DBUS && ENABLE_GIO
    g_bus_own_name_on_connection(m_pConnection, "org.libreoffice.print-dialog",
                                 G_BUS_NAME_OWNER_FLAGS_NONE, onNameAcquired, onNameLost, onStateChanged, this,
                                 nullptr);

    g_dbus_connection_signal_subscribe(m_pConnection, // DBus Connection
                                       nullptr, // Sender Name
                                       "org.openprinting.PrintBackend", // Sender Interface
                                       "PrinterAdded", // Signal Name
                                       nullptr, // Object Path
                                       nullptr, // arg0 behaviour
                                       G_DBUS_SIGNAL_FLAGS_NONE, // Signal Flags
                                       printerAdded, // Callback Function
                                       this, nullptr);
    g_dbus_connection_signal_subscribe(m_pConnection, // DBus Connection
                                       nullptr, // Sender Name
                                       "org.openprinting.PrintBackend", // Sender Interface
                                       "PrinterRemoved", // Signal Name
                                       nullptr, // Object Path
                                       nullptr, // arg0 behaviour
                                       G_DBUS_SIGNAL_FLAGS_NONE, // Signal Flags
                                       printerRemoved, // Callback Function
                                       this, nullptr);

    // remove everything that is not a CUPS printer and not
    // a special purpose printer (PDF, Fax)
    std::unordered_map<OUString, Printer>::iterator it = m_aPrinters.begin();
    while (it != m_aPrinters.end())
    {
        if (m_aCPDDestMap.find(it->first) != m_aCPDDestMap.end())
        {
            ++it;
            continue;
        }

        if (!it->second.m_aInfo.m_aFeatures.isEmpty())
        {
            ++it;
            continue;
        }
        it = m_aPrinters.erase(it);
    }
#endif
}

void CPDManager::setupJobContextData(JobData& rData)
{
#if ENABLE_DBUS && ENABLE_GIO
    std::unordered_map<OUString, CPDPrinter*>::iterator dest_it
        = m_aCPDDestMap.find(rData.m_aPrinterName);

    if (dest_it == m_aCPDDestMap.end())
        return PrinterInfoManager::setupJobContextData(rData);

    std::unordered_map<OUString, Printer>::iterator p_it = m_aPrinters.find(rData.m_aPrinterName);
    if (p_it == m_aPrinters.end()) // huh ?
    {
        SAL_WARN("vcl.unx.print", "CPD printer list in disorder, "
                                  "no dest for printer "
                                      << rData.m_aPrinterName);
        return;
    }

    if (p_it->second.m_aInfo.m_pParser == nullptr)
    {
        // in turn calls createCPDParser
        // which updates the printer info
        p_it->second.m_aInfo.m_pParser = PPDParser::getParser(p_it->second.m_aInfo.m_aDriverName);
    }
    if (p_it->second.m_aInfo.m_aContext.getParser() == nullptr)
    {
        OUString aPrinter;
        if (p_it->second.m_aInfo.m_aDriverName.startsWith("CPD:"))
            aPrinter = p_it->second.m_aInfo.m_aDriverName.copy(4);
        else
            aPrinter = p_it->second.m_aInfo.m_aDriverName;

        p_it->second.m_aInfo.m_aContext = m_aDefaultContexts[aPrinter];
    }

    rData.m_pParser = p_it->second.m_aInfo.m_pParser;
    rData.m_aContext = p_it->second.m_aInfo.m_aContext;
#else
    (void)rData;
#endif
}

FILE* CPDManager::startSpool(const OUString& rPrintername, bool bQuickCommand)
{
#if ENABLE_DBUS && ENABLE_GIO
    SAL_INFO("vcl.unx.print",
             "startSpool: " << rPrintername << " " << (bQuickCommand ? "true" : "false"));
    if (m_aCPDDestMap.find(rPrintername) == m_aCPDDestMap.end())
    {
        SAL_INFO("vcl.unx.print", "defer to PrinterInfoManager::startSpool");
        return PrinterInfoManager::startSpool(rPrintername, bQuickCommand);
    }
    OUString aTmpURL, aTmpFile;
    osl_createTempFile(nullptr, nullptr, &aTmpURL.pData);
    osl_getSystemPathFromFileURL(aTmpURL.pData, &aTmpFile.pData);
    OString aSysFile = OUStringToOString(aTmpFile, osl_getThreadTextEncoding());
    FILE* fp = fopen(aSysFile.getStr(), "w");
    if (fp)
        m_aSpoolFiles[fp] = aSysFile;

    return fp;
#else
    (void)rPrintername;
    (void)bQuickCommand;
    return nullptr;
#endif
}

#if ENABLE_DBUS && ENABLE_GIO
void CPDManager::getOptionsFromDocumentSetup(const JobData& rJob, bool bBanner,
                                             const OString& rJobName, int& rNumOptions,
                                             GVariant** arr)
{
    GVariantBuilder* builder;
    builder = g_variant_builder_new(G_VARIANT_TYPE("a(ss)"));
    g_variant_builder_add(builder, "(ss)", "job-name", rJobName.getStr());
    if (rJob.m_pParser == rJob.m_aContext.getParser() && rJob.m_pParser)
    {
        std::size_t i;
        std::size_t nKeys = rJob.m_aContext.countValuesModified();
        ::std::vector<const PPDKey*> aKeys(nKeys);
        for (i = 0; i < nKeys; i++)
            aKeys[i] = rJob.m_aContext.getModifiedKey(i);
        for (i = 0; i < nKeys; i++)
        {
            const PPDKey* pKey = aKeys[i];
            const PPDValue* pValue = rJob.m_aContext.getValue(pKey);
            OUString sPayLoad;
            if (pValue)
            {
                sPayLoad = pValue->m_bCustomOption ? pValue->m_aCustomOption : pValue->m_aOption;
            }
            if (!sPayLoad.isEmpty())
            {
                OString aKey = OUStringToOString(pKey->getKey(), RTL_TEXTENCODING_ASCII_US);
                OString aValue = OUStringToOString(sPayLoad, RTL_TEXTENCODING_ASCII_US);
                if (aKey.equals("Duplex"_ostr))
                {
                    aKey = "sides"_ostr;
                }
                else if (aKey.equals("Resolution"_ostr))
                {
                    aKey = "printer-resolution"_ostr;
                }
                else if (aKey.equals("PageSize"_ostr))
                {
                    aKey = "media"_ostr;
                }
                if (aKey.equals("sides"_ostr))
                {
                    if (aValue.equals("None"_ostr))
                    {
                        aValue = "one-sided"_ostr;
                    }
                    else if (aValue.equals("DuplexNoTumble"_ostr))
                    {
                        aValue = "two-sided-long-edge"_ostr;
                    }
                    else if (aValue.equals("DuplexTumble"_ostr))
                    {
                        aValue = "two-sided-short-edge"_ostr;
                    }
                }
                g_variant_builder_add(builder, "(ss)", aKey.getStr(), aValue.getStr());
            }
        }
    }
    if (rJob.m_nCopies > 1)
    {
        OString aVal(OString::number(rJob.m_nCopies));
        g_variant_builder_add(builder, "(ss)", "copies", aVal.getStr());
        rNumOptions++;
        // TODO: something for collate
        // Maybe this is the equivalent ipp attribute:
        if (rJob.m_bCollate)
        {
            g_variant_builder_add(builder, "(ss)", "multiple-document-handling",
                                  "separate-documents-collated-copies");
        }
        else
        {
            g_variant_builder_add(builder, "(ss)", "multiple-document-handling",
                                  "separate-documents-uncollated-copies");
        }
        rNumOptions++;
    }
    if (!bBanner)
    {
        g_variant_builder_add(builder, "(ss)", "job-sheets", "none");
        rNumOptions++;
    }
    if (rJob.m_eOrientation == orientation::Portrait)
    {
        g_variant_builder_add(builder, "(ss)", "orientation-requested", "portrait");
        rNumOptions++;
    }
    else if (rJob.m_eOrientation == orientation::Landscape)
    {
        g_variant_builder_add(builder, "(ss)", "orientation-requested", "landscape");
        rNumOptions++;
    }
    (*arr) = g_variant_new("a(ss)", builder);
    g_variant_builder_unref(builder);
}
#endif

bool CPDManager::endSpool(const OUString& rPrintername, const OUString& rJobTitle, FILE* pFile,
                          const JobData& rDocumentJobData, bool bBanner, const OUString& rFaxNumber)
{
    bool success = false;
#if ENABLE_DBUS && ENABLE_GIO
    SAL_INFO("vcl.unx.print", "endSpool: " << rPrintername << "," << rJobTitle
                                           << " copy count = " << rDocumentJobData.m_nCopies);
    std::unordered_map<OUString, CPDPrinter*>::iterator dest_it = m_aCPDDestMap.find(rPrintername);
    if (dest_it == m_aCPDDestMap.end())
    {
        SAL_INFO("vcl.unx.print", "defer to PrinterInfoManager::endSpool");
        return PrinterInfoManager::endSpool(rPrintername, rJobTitle, pFile, rDocumentJobData,
                                            bBanner, rFaxNumber);
    }

    std::unordered_map<FILE*, OString, FPtrHash>::const_iterator it = m_aSpoolFiles.find(pFile);
    if (it != m_aSpoolFiles.end())
    {
        fclose(pFile);
        rtl_TextEncoding aEnc = osl_getThreadTextEncoding();
        OString sJobName(OUStringToOString(rJobTitle, aEnc));
        if (!rFaxNumber.isEmpty())
        {
            sJobName = OUStringToOString(rFaxNumber, aEnc);
        }
        OString aSysFile = it->second;
        CPDPrinter* pDest = dest_it->second;
        GVariant* ret;
        gint job_id;
        int nNumOptions = 0;
        GVariant* pArr = nullptr;
        getOptionsFromDocumentSetup(rDocumentJobData, bBanner, sJobName, nNumOptions, &pArr);
        ret = g_dbus_proxy_call_sync(
            pDest->backend, "printFile",
            g_variant_new("(ssi@a(ss))", (pDest->id), aSysFile.getStr(), nNumOptions, pArr),
            G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
        g_variant_get(ret, "(i)", &job_id);
        if (job_id != -1)
        {
            success = true;
        }
        g_variant_unref(ret);
        unlink(it->second.getStr());
        m_aSpoolFiles.erase(it);
    }
#else
    (void)rPrintername;
    (void)rJobTitle;
    (void)pFile;
    (void)rDocumentJobData;
    (void)bBanner;
    (void)rFaxNumber;
#endif
    return success;
}

bool CPDManager::checkPrintersChanged(bool)
{
#if ENABLE_DBUS && ENABLE_GIO
    bool bChanged = m_aPrintersChanged;
    m_aPrintersChanged = false;
    g_dbus_connection_emit_signal(m_pConnection, nullptr, "/org/libreoffice/PrintDialog",
                                  "org.openprinting.PrintFrontend", "RefreshBackend", nullptr,
                                  nullptr);
    return bChanged;
#else
    return false;
#endif
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
