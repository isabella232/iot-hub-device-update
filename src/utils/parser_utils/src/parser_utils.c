/**
 * @file parser_utils.c
 * @brief Implements utilities for parsing the common data types.
 *
 * @copyright Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 */
#include "aduc/parser_utils.h"
#include "aduc/hash_utils.h"
#include "aduc/logging.h"
#include "parson_json_utils.h"

#include <azure_c_shared_utility/crt_abstractions.h> // for mallocAndStrcpy_s
#include <stdlib.h> // for calloc

/**
 * @brief Retrieves the updateManifest from the updateActionJson
 * @details Caller must free the returned JSON_Value with Parson's json_value_free
 * @param updateActionJson UpdateAction JSON to parse.
 * @return NULL in case of failure, the updateManifest JSON Value on success
 */
JSON_Value* ADUC_JSON_GetUpdateManifestRoot(const JSON_Value* updateActionJson)
{
    char* manifestString = NULL;
    if (!ADUC_JSON_GetStringField(updateActionJson, ADUCITF_FIELDNAME_UPDATEMANIFEST, &manifestString))
    {
        Log_Error("updateActionJson does not include an updateManifest field");
        return false;
    }

    return json_parse_string(manifestString);
}

/**
 * @brief Allocates memory and populate with ADUC_Hash object from a Parson JSON_Object.
 *
 * Caller MUST assume that this method allocates the memory for the returned ADUC_Hash pointer.
 *
 * @param hashObj JSON Object that contains the hashes to be returned.
 * @param hashCount A size_t* where the count of output hashes will be stored.
 * @returns If success, a pointer to an array of ADUC_Hash object. Otherwise, returns NULL.
 *  Caller must call ADUC_FileEntityArray_Free() to free the array.
 */
ADUC_Hash* ADUC_HashArray_AllocAndInit(const JSON_Object* hashObj, size_t* hashCount)
{
    _Bool success = false;

    ADUC_Hash* tempHashArray = NULL;

    if (hashCount == NULL)
    {
        return false;
    }
    *hashCount = 0;

    size_t tempHashCount = json_object_get_count(hashObj);

    if (tempHashCount == 0)
    {
        Log_Error("No hashes.");
        goto done;
    }

    tempHashArray = (ADUC_Hash*)calloc(tempHashCount, sizeof(ADUC_Hash));

    if (tempHashArray == NULL)
    {
        goto done;
    }

    for (size_t hash_index = 0; hash_index < tempHashCount; ++hash_index)
    {
        ADUC_Hash* currHash = tempHashArray + hash_index;

        const char* hashType = json_object_get_name(hashObj, hash_index);
        const char* hashValue = json_value_get_string(json_object_get_value_at(hashObj, hash_index));
        if (!ADUC_Hash_Init(currHash, hashValue, hashType))
        {
            goto done;
        }
    }

    *hashCount = tempHashCount;

    success = true;

done:

    if (!success)
    {
        ADUC_Hash_FreeArray(tempHashCount, tempHashArray);
        tempHashArray = NULL;
        tempHashCount = 0;
    }

    *hashCount = tempHashCount;

    return tempHashArray;
}

/**
 * @brief Free memory allocated for the specified ADUC_FileEntity object's member.
 *
 * @param entity File entity to be freed.
 */
void ADUC_FileEntity_Uninit(ADUC_FileEntity* entity)
{
    if (entity == NULL)
    {
        return;
    }
    free(entity->DownloadUri);
    free(entity->TargetFilename);
    free(entity->FileId);
    free(entity->Arguments);
    ADUC_Hash_FreeArray(entity->HashCount, entity->Hash);
    memset(entity, 0, sizeof(*entity));
}

/**
 * @brief Free ADUC_FileEntity object array.
 *
 * Caller should assume files object is invalid after this method returns.
 *
 * @param fileCount Count of objects in files.
 * @param files Array of ADUC_FileEntity objects to free.
 */
void ADUC_FileEntityArray_Free(unsigned int fileCount, ADUC_FileEntity* files)
{
    for (unsigned int index = 0; index < fileCount; ++index)
    {
        ADUC_FileEntity_Uninit(files + index);
    }

    free(files);
}

/**
 * @brief Initializes the file entity
 * @param file the file entity to be initialized
 * @param fileId fileId for @p fileEntity
 * @param targetFileName fileName for @p fileEntity
 * @param downloadUri downloadUri for @p fileEntity
 * @param arguments arguments for @p fileEntity (payload for down-level update handler)
 * @param hashArray a hash array for @p fileEntity
 * @param hashCount a hash count of @p hashArray
 * @param sizeInBytes file size (in bytes)
 * @returns True on success and false on failure
 */
_Bool ADUC_FileEntity_Init(
    ADUC_FileEntity* fileEntity,
    const char* fileId,
    const char* targetFileName,
    const char* downloadUri,
    const char* arguments,
    ADUC_Hash* hashArray,
    size_t hashCount,
    size_t sizeInBytes)
{
    _Bool success = false;

    if (fileEntity == NULL)
    {
        return false;
    }

    // Note: downloadUri could be empty when the agent resuming 'install' or 'apply' action.
    if (fileId == NULL || targetFileName == NULL || hashArray == NULL)
    {
        return false;
    }

    memset(fileEntity, 0, sizeof(*fileEntity));

    if (mallocAndStrcpy_s(&(fileEntity->FileId), fileId) != 0)
    {
        goto done;
    }

    if (mallocAndStrcpy_s(&(fileEntity->TargetFilename), targetFileName) != 0)
    {
        goto done;
    }

    if (downloadUri == NULL)
    {
        fileEntity->DownloadUri = NULL;
    }
    else if (mallocAndStrcpy_s(&(fileEntity->DownloadUri), downloadUri) != 0)
    {
        goto done;
    }

    if (arguments != NULL && mallocAndStrcpy_s(&(fileEntity->Arguments), arguments) != 0)
    {
        goto done;
    }

    fileEntity->Hash = hashArray;

    fileEntity->HashCount = hashCount;

    fileEntity->SizeInBytes = sizeInBytes;

    success = true;
done:

    if (!success)
    {
        ADUC_FileEntity_Uninit(fileEntity);
    }
    return success;
}

/**
 * @brief Parse the update action JSON for the UpdateId value.
 *
 * Sample JSON:
 * {
 *      "updateManifest":"{
 *      ...
 *      \"updateId\": {
 *          \"provider\": \"Azure\",
 *          \"name\":\"IOT-Firmware\",
 *          \"version\":\"1.2.0.0\"
 *      },
 *      ...
 *     }",
 *
 * }
 *
 * @param updateActionJson UpdateAction JSON to parse.
 * @param updateId The returned installed content ID string. Caller must call free().
 * @return _Bool True if call was successful.
 */
_Bool ADUC_Json_GetUpdateId(const JSON_Value* updateActionJson, ADUC_UpdateId** updateId)
{
    _Bool success = false;
    ADUC_UpdateId* tempUpdateID = NULL;

    *updateId = NULL;

    JSON_Value* updateManifestValue = ADUC_JSON_GetUpdateManifestRoot(updateActionJson);

    if (updateManifestValue == NULL)
    {
        Log_Error("updateManifest JSON is invalid");
        goto done;
    }

    JSON_Object* updateManifestObj = json_value_get_object(updateManifestValue);

    if (updateManifestObj == NULL)
    {
        Log_Error("updateManifestValue is not a JSON Object");
        goto done;
    }

    JSON_Value* updateIdValue = json_object_get_value(updateManifestObj, ADUCITF_FIELDNAME_UPDATEID);

    if (updateIdValue == NULL)
    {
        Log_Error("updateActionJson's updateManifest does not include an updateid field");
        goto done;
    }

    const char* provider = ADUC_JSON_GetStringFieldPtr(updateIdValue, ADUCITF_FIELDNAME_PROVIDER);
    const char* name = ADUC_JSON_GetStringFieldPtr(updateIdValue, ADUCITF_FIELDNAME_NAME);
    const char* version = ADUC_JSON_GetStringFieldPtr(updateIdValue, ADUCITF_FIELDNAME_VERSION);

    if (provider == NULL || name == NULL || version == NULL)
    {
        Log_Error("Invalid json. Missing required UpdateID fields");
        goto done;
    }

    tempUpdateID = ADUC_UpdateId_AllocAndInit(provider, name, version);
    if (tempUpdateID == NULL)
    {
        goto done;
    }

    success = true;

done:
    if (!success)
    {
        ADUC_UpdateId_UninitAndFree(tempUpdateID);
        tempUpdateID = NULL;
    }

    json_value_free(updateManifestValue);

    *updateId = tempUpdateID;
    return success;
}

/**
 * @brief Parse the update action JSON into a ADUC_FileEntity structure.
 *
 * Sample JSON:
 *
 * {
 *     ...,
 *     "updateManifest" : {
 *          ...,
 *
 *         "files": {
 *             "0001": {
 *                  "fileName": "fileName",
 *                  "sizeInBytes": "1024",
 *                  "hashes":{
 *                      "sha256": "base64_encoded_hash_value"
 *                   }
 *              },
 *         }
 *         ...
 *      },
 *      ...
 *      "fileUrls": {
 *          "0001": "uri1"
 *       },
 *       ...
 * }
 *
 * @param updateActionJson UpdateAction Json to parse
 * @param fileCount Returned number of files.
 * @param files ADUC_FileEntity (size fileCount). Array to be freed using free(), objects must also be freed.
 * @return _Bool Success state.
 */
_Bool ADUC_Json_GetFiles(const JSON_Value* updateActionJson, unsigned int* fileCount, ADUC_FileEntity** files)
{
    _Bool succeeded = false;

    if (fileCount == NULL || files == NULL)
    {
        return false;
    }

    *fileCount = 0;
    *files = NULL;

    JSON_Value* updateManifestValue = ADUC_JSON_GetUpdateManifestRoot(updateActionJson);

    if (updateManifestValue == NULL)
    {
        goto done;
    }

    const JSON_Object* updateManifest = json_value_get_object(updateManifestValue);
    const JSON_Object* filesObject = json_object_get_object(updateManifest, ADUCITF_FIELDNAME_FILES);

    if (filesObject == NULL)
    {
        Log_Error("Invalid json - '%s' missing or incorrect", ADUCITF_FIELDNAME_FILES);
        goto done;
    }

    // Get file count from 'UpdateManifest' property.
    const size_t filesCount = json_object_get_count(filesObject);
    if (filesCount == 0)
    {
        Log_Error("An update manifest must contain at least one file.");
        goto done;
    }

    const JSON_Object* updateActionJsonObject = json_value_get_object(updateActionJson);
    const JSON_Object* fileUrlsObject = json_object_get_object(updateActionJsonObject, ADUCITF_FIELDNAME_FILE_URLS);
    const size_t fileUrlsCount = json_object_get_count(fileUrlsObject);

    if (fileUrlsCount == 0)
    {
        Log_Error("File URLs is empty.");
        goto done;
    }

    // Previously, we're expecting UpdateManifest.files.count to match UpdateAction.fileUrls.count.
    // This is no-longer a valid expectation.
    // For 'microsoft/bundle:*' update type, UpdateManifest.files contains only a list of 'microsoft/components:*' manifest files.
    // However, the UpdateAction.fileUrls contains all files referenced by both Bundle and Components Updates.
    if (fileUrlsCount < filesCount)
    {
        Log_Error("File URLs count (%d) is less than UpdateManifest's Files count (%d).", fileUrlsCount, filesCount);
        goto done;
    }

    *files = calloc(filesCount, sizeof(ADUC_FileEntity));
    if (*files == NULL)
    {
        goto done;
    }

    *fileCount = filesCount;

    for (size_t index = 0; index < filesCount; ++index)
    {
        ADUC_FileEntity* curFile = *files + index;

        const JSON_Object* fileObj = json_value_get_object(json_object_get_value_at(filesObject, index));
        const JSON_Object* hashObj = json_object_get_object(fileObj, ADUCITF_FIELDNAME_HASHES);

        if (hashObj == NULL)
        {
            Log_Error("No hash for file @ %zu", index);
            goto done;
        }
        size_t tempHashCount = 0;
        ADUC_Hash* tempHash = ADUC_HashArray_AllocAndInit(hashObj, &tempHashCount);
        if (tempHash == NULL)
        {
            Log_Error("Unable to parse hashes for file @ %zu", index);
            goto done;
        }

        const char* uri = json_value_get_string(json_object_get_value_at(fileUrlsObject, index));
        const char* fileId = json_object_get_name(filesObject, index);
        const char* name = json_object_get_string(fileObj, ADUCITF_FIELDNAME_FILENAME);
        const char* arguments = json_object_get_string(fileObj, ADUCITF_FIELDNAME_ARGUMENTS);
        size_t sizeInBytes = 0;
        if (json_object_has_value(fileObj, ADUCITF_FIELDNAME_SIZEINBYTES))
        {
            sizeInBytes = json_object_get_number(fileObj, ADUCITF_FIELDNAME_SIZEINBYTES);
        }

        if (!ADUC_FileEntity_Init(curFile, fileId, name, uri, arguments, tempHash, tempHashCount, sizeInBytes))
        {
            ADUC_Hash_FreeArray(tempHashCount, tempHash);
            Log_Error("Invalid file arguments");
            goto done;
        }
    }

    succeeded = true;

done:
    if (!succeeded)
    {
        ADUC_FileEntityArray_Free(*fileCount, *files);
        *files = NULL;
        *fileCount = 0;
    }

    json_value_free(updateManifestValue);

    return succeeded;
}
