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
extern "C"{
static void printerUpdateCallback(cpdb_frontend_obj_t* frontendObj,
                      cpdb_printer_obj_t* pDest,
                      cpdb_printer_update_t change) {
    switch (change) {
        case CPDB_CHANGE_PRINTER_ADDED:
        g_message("Added printer %s : %s!\n", pDest->name, pDest->backend_name);
        break;

        case CPDB_CHANGE_PRINTER_REMOVED:
        g_message("Removed printer %s : %s!\n", pDest->name, pDest->backend_name);
        cpdbDeletePrinterObj(pDest);
        break;

        case CPDB_CHANGE_PRINTER_STATE_CHANGED:
        g_message("Printer state changed for %s : %s to \"%s\"", pDest->name,
                    pDest->backend_name, pDest->state);
        break;
    }
}
}
// Function to execute when name is acquired on the bus
void CPDManager::onNameAcquired(GDBusConnection* connection, const gchar*, gpointer user_data)
{
    GError *error = NULL;
    cpdb_frontend_obj_t *frontendObj = static_cast<cpdb_frontend_obj_t *> (user_data);

    g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(frontendObj->skeleton),
                                     connection, 
                                     CPDB_DIALOG_OBJ_PATH,
                                     &error);
    if (error)
    {
        g_message("Error exporting frontend interface : %s\n", error->message);
        return;
    }
    
    cpdbActivateBackends(frontendObj);
    frontendObj->name_done = TRUE;
}

void CPDManager::onNameLost(GDBusConnection* connection, const gchar* name, gpointer user_data)
{
    g_message("Name Lost: %s", name);
    cpdb_frontend_obj_t *frontendObj = static_cast<cpdb_frontend_obj_t *> (user_data);
    frontendObj->name_done = TRUE;
}

void CPDManager::printerStateChanged(GDBusConnection* connection, const gchar* sender_name,
                              const gchar* object_path, const gchar* interface_name, const gchar* signal_name,
                              GVariant* parameters, gpointer user_data){
    //Implement state changed signal
    cpdb_frontend_obj_t *frontendObj = static_cast<cpdb_frontend_obj_t *> (user_data);
    gboolean printer_is_accepting_jobs;
    char *printer_id, *printer_state, *backend_name;

    g_variant_get(parameters, "(ssbs)", &printer_id, &printer_state,
                    &printer_is_accepting_jobs, &backend_name);
    cpdb_printer_obj_t *pDest = cpdbFindPrinterObj(frontendObj, printer_id, backend_name);
    if (pDest->state)
        free(pDest->state);
    pDest->state = cpdbGetStringCopy(printer_state);
    pDest->accepting_jobs = printer_is_accepting_jobs;
    frontendObj->printer_cb(frontendObj, pDest, CPDB_CHANGE_PRINTER_STATE_CHANGED);
}

void CPDManager::fillBasicOptions(cpdb_printer_obj_t *pDest,
                          GVariant *parameters)
{
    g_variant_get(parameters, CPDB_PRINTER_ADDED_ARGS,
                  &(pDest->id),
                  &(pDest->name),
                  &(pDest->info),
                  &(pDest->location),
                  &(pDest->make_and_model),
                  &(pDest->accepting_jobs),
                  &(pDest->state),
                  &(pDest->backend_name));
}

void CPDManager::printerAdded(GDBusConnection* connection, const gchar* sender_name,
                              const gchar* object_path, const gchar* interface_name, const gchar* signal_name,
                              GVariant* parameters, gpointer user_data)
{
    cpdb_frontend_obj_t *frontendObj = static_cast<cpdb_frontend_obj_t *> (user_data);
    cpdb_printer_obj_t *pDest = cpdbGetNewPrinterObj();
    
    if (frontendObj->last_saved_settings != NULL)
    {
        cpdbCopySettings(frontendObj->last_saved_settings, pDest->settings);
    }
    current->fillBasicOptions(pDest, parameters);
    cpdbAddPrinter(frontendObj, pDest);
    frontendObj->printer_cb(frontendObj, pDest, CPDB_CHANGE_PRINTER_ADDED);
    std::stringstream printerName;
    printerName << pDest->name << ", " << pDest->backend_name;
    std::stringstream uniqueName;
    uniqueName << pDest->id << ", " << pDest->backend_name;
    rtl_TextEncoding aEncoding = osl_getThreadTextEncoding();
    OUString aPrinterName = OStringToOUString(printerName.str(), aEncoding);
    OUString aUniqueName = OStringToOUString(uniqueName.str(), aEncoding);
    current->addNewPrinter(aPrinterName, aUniqueName, pDest);
}

void CPDManager::printerRemoved(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                const gchar*, GVariant* parameters, gpointer user_data)
{
    cpdb_frontend_obj_t *frontendObj = static_cast<cpdb_frontend_obj_t *> (user_data);
    char *printer_id;
    char *backend_name;
    
    g_variant_get(parameters, "(ss)", &printer_id, &backend_name);
    cpdb_printer_obj_t *pDest = cpdbRemovePrinter(frontendObj, printer_id, backend_name);
    frontendObj->printer_cb(frontendObj, pDest, CPDB_CHANGE_PRINTER_REMOVED);
    std::stringstream uniqueName;
    uniqueName << printer_id << ", " << backend_name;
    rtl_TextEncoding aEncoding = osl_getThreadTextEncoding();
    OUString aUniqueName = OStringToOUString(uniqueName.str(), aEncoding);
    std::unordered_map<OUString, cpdb_printer_obj_t*>::iterator it
        = pManager->m_aCPDDestMap.find(aUniqueName);
    if (it == pManager->m_aCPDDestMap.end())
    {
        SAL_WARN("vcl.unx.print", "CPD trying to remove non-existent printer from list");
        return;
    }
    pManager->m_aCPDDestMap.erase(it);
    std::unordered_map<OUString, Printer>::iterator printersIt
        = pManager->m_aPrinters.find(aUniqueName);
    if (printersIt == pManager->m_aPrinters.end())
    {
        SAL_WARN("vcl.unx.print", "CPD trying to remove non-existent printer from m_aPrinters");
        return;
    }
    pManager->m_aPrinters.erase(printersIt);
}

void CPDManager::addNewPrinter(const OUString& aPrinterName, const OUString& aUniqueName,
                               cpdb_printer_obj_t* pDest)
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
        pManager = new CPDManager();
    }
#endif
    return pManager;
}

CPDManager::CPDManager()
    : PrinterInfoManager(PrinterInfoManager::Type::CPD)
{
#if ENABLE_DBUS && ENABLE_GIO
    cpdbInit();
    cpdb_printer_callback printerCb = static_cast<cpdb_printer_callback>(printerUpdateCallback);
    const char *instanceName = "CPD";
    frontendObj = cpdbGetNewFrontendObj(instanceName, printerCb);
    cpdbConnectToDBus(frontendObj);
    cpdbIgnoreLastSavedSettings(frontendObj);
#endif
}

CPDManager::~CPDManager()
{
#if ENABLE_DBUS && ENABLE_GIO
    cpdbDeleteFrontendObj(frontendObj);
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

    std::unordered_map<OUString, cpdb_printer_obj_t*>::iterator dest_it = m_aCPDDestMap.find(aPrinter);

    // TODO: These keys need to be redefined to preserve usage across libreoffice
    // InputSlot - media-col.media-source?
    // Font - not needed now as it is required only for ps and we are using pdf
    // Dial? - for FAX (need to look up PWG spec)
    
    if (dest_it != m_aCPDDestMap.end())
    {
        cpdb_printer_obj_t* pDest = dest_it->second;
        cpdb_options_t* options = cpdbGetAllOptions(pDest);

        if (options != nullptr && options->count > 0)
        {
            rtl_TextEncoding aEncoding = osl_getThreadTextEncoding();
            PPDKey* pKey = nullptr;
            OUString aValueName;
            PPDValue* pValue;
            std::vector<PPDKey*> keys;
            std::vector<OUString> default_values;

            GHashTableIter iter;
            gpointer key, value;
            g_hash_table_iter_init(&iter, options->table);
            while (g_hash_table_iter_next(&iter, &key, &value))
            {
                cpdb_option_t* option = static_cast<cpdb_option_t*>(value);
                OUString aOptionName = OStringToOUString(option->option_name, aEncoding);
                OUString aDefaultValue = OStringToOUString(option->default_value, aEncoding);

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
                if (option->num_supported > 0 && aDefaultValue != "NA")
                    pKey->m_bUIOption = true;

                bool bDefaultFound = false;

                for (int j = 0; j < option->num_supported; ++j)
                {
                    aValueName = OStringToOUString(option->supported_values[j], aEncoding);
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

                if (!bDefaultFound && pKey->m_bUIOption)
                {
                    // Uncomment if needed to ensure default values appear as options:
                    // pValue = pKey->insertValue(aDefaultValue, eQuoted);
                    // if (pValue)
                    //     pValue->m_aValue = aDefaultValue;
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
                        SAL_INFO("vcl.unx.print", "key " << pKey->getKey() << " is set to " << *defit);
                    }
                    else
                        SAL_INFO("vcl.unx.print", "key " << pKey->getKey() << " is defaulted to " << *defit);
                }
                ++defit;
            }

            rInfo.m_pParser = pNewParser;
            rInfo.m_aContext = rContext;

            cpdbFreeOptions(options);  // Ensure to free the options after use
        }
        else
        {
            g_clear_error(&error);
            SAL_INFO("vcl.unx.print", "CPD GetAllOptions failed, falling back to generic driver");
        }
    }
    else
    {
        SAL_INFO("vcl.unx.print", "no dest found for printer " << aPrinter);
    }

    if (!pNewParser)
    {
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

void CPDManager::addPrinters(gpointer key, gpointer value, gpointer user_data){
    cpdb_printer_obj_t* pDest = static_cast<cpdb_printer_obj_t*>(value);

    std::stringstream printerName;
    printerName << pDest->name << ", " << pDest->backend_name;

    std::stringstream uniqueName;
    uniqueName << pDest->id << ", " << pDest->backend_name;

    rtl_TextEncoding aEncoding = osl_getThreadTextEncoding();
    OUString aPrinterName = OStringToOUString(printerName.str(), aEncoding);
    OUString aUniqueName = OStringToOUString(uniqueName.str(), aEncoding);

    addNewPrinter(aPrinterName, aUniqueName, pDest);
}

void CPDManager::initialize()
{
    // get normal printers, clear printer list
    PrinterInfoManager::initialize();
#if ENABLE_DBUS && ENABLE_GIO
    // add CPDB printers, should there be a printer
    // with the same name as a CPDB printer, overwrite it
    g_hash_table_foreach(frontendObj->printer, addPrinters, nullptr);
    // remove everything that is not a CPD printer and not
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
    std::unordered_map<OUString, cpdb_printer_obj_t*>::iterator dest_it
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
cpdb_settings_t CPDManager::getOptionsFromDocumentSetup(const JobData& rJob, bool bBanner,
                                             const OString& rJobName, int& rNumOptions)
{

    cpdb_settings_t settings;
    // Add the job name to the hash table
    g_hash_table_insert(settings->table, g_strdup("job-name"), g_strdup(rJobName.getStr()));

    if (rJob.m_pParser == rJob.m_aContext.getParser() && rJob.m_pParser)
    {
        std::size_t i;
        std::size_t nKeys = rJob.m_aContext.countValuesModified();
        std::vector<const PPDKey*> aKeys(nKeys);
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
                g_hash_table_insert(settings->table, g_strdup(aKey.getStr()), g_strdup(aValue.getStr()));
            }
        }
    }

    if (rJob.m_nCopies > 1)
    {
        OString aVal(OString::number(rJob.m_nCopies));
        g_hash_table_insert(settings->table, g_strdup("copies"), g_strdup(aVal.getStr()));
        rNumOptions++;

        // TODO: something for collate
        // Maybe this is the equivalent ipp attribute:
        if (rJob.m_bCollate)
        {
            g_hash_table_insert(settings->table, g_strdup("multiple-document-handling"), g_strdup("separate-documents-collated-copies"));
        }
        else
        {
            g_hash_table_insert(settings->table, g_strdup("multiple-document-handling"), g_strdup("separate-documents-uncollated-copies"));
        }
        rNumOptions++;
    }

    if (!bBanner)
    {
        g_hash_table_insert(settings->table, g_strdup("job-sheets"), g_strdup("none"));
        rNumOptions++;
    }

    if (rJob.m_eOrientation == orientation::Portrait)
    {
        g_hash_table_insert(settings->table, g_strdup("orientation-requested"), g_strdup("portrait"));
        rNumOptions++;
    }
    else if (rJob.m_eOrientation == orientation::Landscape)
    {
        g_hash_table_insert(settings->table, g_strdup("orientation-requested"), g_strdup("landscape"));
        rNumOptions++;
    }
    settings->count = rNumOptions;
    return settings;
}
#endif

bool CPDManager::endSpool(const OUString& rPrintername, const OUString& rJobTitle, FILE* pFile,
                          const JobData& rDocumentJobData, bool bBanner, const OUString& rFaxNumber)
{
    bool success = false;
#if ENABLE_DBUS && ENABLE_GIO
    SAL_INFO("vcl.unx.print", "endSpool: " << rPrintername << "," << rJobTitle
                                           << " copy count = " << rDocumentJobData.m_nCopies);
    std::unordered_map<OUString, cpdb_printer_obj_t*>::iterator dest_it = m_aCPDDestMap.find(rPrintername);
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
        cpdb_printer_obj_t pDest = dest_it->second;
        GVariant* ret;
        int nNumOptions = 0;
        cpdb_settings_t settings = getOptionsFromDocumentSetup(rDocumentJobData, bBanner, sJobName, nNumOptions);
        pDest->settings = settings;
        char *id = cpdbPrintFile(pDest, aSysFile.getStr());
        int job_id = std::stoi(id);
        if (job_id != -1)
        {
            success = true;
        }
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

void CPDManager::updatePrinters(gpointer key, gpointer value, gpointer user_data){
    cpdb_printer_obj_t* pDest = static_cast<cpdb_printer_obj_t*>(value);

    std::stringstream printerName;
    printerName << pDest->name << ", " << pDest->backend_name;

    std::stringstream uniqueName;
    uniqueName << pDest->id << ", " << pDest->backend_name;

    rtl_TextEncoding aEncoding = osl_getThreadTextEncoding();
    OUString aPrinterName = OStringToOUString(printerName.str(), aEncoding);
    OUString aUniqueName = OStringToOUString(uniqueName.str(), aEncoding);

    // Check if the unique name is already in the map
    if (m_aPrinters.find(aUniqueName) == m_aPrinters.end()) {
        addNewPrinter(aPrinterName, aUniqueName, pDest);
    }
}

bool CPDManager::checkPrintersChanged(bool)
{
#if ENABLE_DBUS && ENABLE_GIO
    int prev = g_hash_table_size(frontendObj->printer);
    cpdbActivateBackends(frontendObj);
    int curr = g_hash_table_size(frontendObj->printer);

    if(prev!=curr) {
        g_hash_table_foreach(frontendObj->printer, updatePrinters, nullptr);
        return true;
    }
    return false;
#else
    return false;
#endif
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
