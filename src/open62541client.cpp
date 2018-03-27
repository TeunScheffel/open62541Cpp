/*
 * Copyright (C) 2017 -  B. J. Hill
 *
 * This file is part of open62541 C++ classes. open62541 C++ classes are free software: you can
 * redistribute it and/or modify it under the terms of the Mozilla Public
 * License v2.0 as stated in the LICENSE file provided with open62541.
 *
 * open62541 C++ classes are distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.
 */
#include "open62541client.h"
#include "clientbrowser.h"

/*!
 * \brief Open62541::Client::subscriptionInactivityCallback
 * \param client
 * \param subId
 * \param subContext
 */
void  Open62541::Client::subscriptionInactivityCallback (UA_Client *client, UA_UInt32 subId, void *subContext)
{
    Client *p =   (Client *)(UA_Client_getContext(client));
    if(p)
    {
        p->subscriptionInactivity(subId,subContext);
    }
}

/*!
 * \brief Open62541::Client::stateCallback
 * \param client
 * \param clientState
 */
void  Open62541::Client::stateCallback (UA_Client *client, UA_ClientState clientState)
{
    Client *p =   (Client *)(UA_Client_getContext(client));
    if(p)
    {
        p->stateChange(clientState);
    }
}

/*!
 * \brief Open62541::Client::deleteSubscriptionCallback
 * \param client
 * \param subscriptionId
 * \param subscriptionContext
 */
void  Open62541::Client::deleteSubscriptionCallback(UA_Client *client, UA_UInt32 subscriptionId, void *subscriptionContext)
{
    Client *p =   (Client *)(UA_Client_getContext(client));
    if(p)
    {
        p->deleteSubscription(subscriptionId,subscriptionContext);
    }
}


/*!
    \brief Open62541::Client::deleteTree
    \param nodeId
    \return
*/
bool Open62541::Client::deleteTree(NodeId &nodeId) {
    NodeIdMap m;
    browseTree(nodeId, m);
    for (auto i = m.begin(); i != m.end(); i++) {
        UA_NodeId &ni =  i->second;
        if (ni.namespaceIndex > 0) { // namespace 0 appears to be reserved
            //std::cerr  << "Delete " << i->first << std::endl;
            WriteLock l(_mutex);
            UA_Client_deleteNode(_client, i->second, true);
        }
    }
    return lastOK();
}

/*!
    \brief browseTreeCallBack
    \param childId
    \param isInverse
    \param referenceTypeId
    \param handle
    \return
*/

static UA_StatusCode browseTreeCallBack(UA_NodeId childId, UA_Boolean isInverse, UA_NodeId /*referenceTypeId*/, void *handle) {
    if (!isInverse) { // not a parent node - only browse forward
        Open62541::UANodeIdList  *pl = (Open62541::UANodeIdList *)handle;
        pl->put(childId);
    }
    return UA_STATUSCODE_GOOD;
}

/*!
    \brief Open62541::Client::browseChildren
    \param nodeId
    \param m
    \return
*/
bool Open62541::Client::browseChildren(UA_NodeId &nodeId, NodeIdMap &m) {
    Open62541::UANodeIdList l;
    {
        WriteLock ll(mutex());
        UA_Client_forEachChildNodeCall(_client, nodeId,  browseTreeCallBack, &l); // get the childlist
    }
    for (int i = 0; i < int(l.size()); i++) {
        if (l[i].namespaceIndex == nodeId.namespaceIndex) { // only in same namespace
            std::string s = Open62541::toString(l[i]);
            if (m.find(s) == m.end()) {
                m.put(l[i]);
                browseChildren(l[i], m); // recurse no duplicates
            }
        }
    }
    return lastOK();
}

/*!
    \brief Open62541::Client::browseTree
    \param nodeId
    \param tree
    \return
*/
bool Open62541::Client::browseTree(Open62541::NodeId &nodeId, Open62541::UANodeTree &tree) {
    // form a heirachical tree of nodes given node is added to tree
    tree.root().setData(nodeId); // set the root of the tree
    return browseTree(nodeId.get(), tree.rootNode());
}

/*!
    \brief Open62541::Client::browseTree
    \param nodeId
    \param node
    \return
*/
bool Open62541::Client::browseTree(UA_NodeId &nodeId, Open62541::UANode *node) {
    // form a heirachical tree of nodes
    Open62541::UANodeIdList l;
    {
        WriteLock ll(mutex());
        UA_Client_forEachChildNodeCall(_client, nodeId,  browseTreeCallBack, &l); // get the childlist
    }
    for (int i = 0; i < int(l.size()); i++) {
        if (l[i].namespaceIndex > 0) {
            QualifiedName outBrowseName;
            {
                WriteLock ll(mutex());
                _lastError = __UA_Client_readAttribute(_client, &l[i], UA_ATTRIBUTEID_BROWSENAME, outBrowseName, &UA_TYPES[UA_TYPES_QUALIFIEDNAME]);
            }
            if (lastOK()) {
                std::string s = toString(outBrowseName.get().name); // get the browse name and leaf key
                NodeId nId = l[i]; // deep copy
                UANode *n = node->createChild(s); // create the node
                n->setData(nId);
                browseTree(l[i], n);
            }
        }
    }
    return lastOK();
}

/*!
    \brief Open62541::Client::browseTree
    \param nodeId
    \param tree
    \return
*/
bool Open62541::Client::browseTree(NodeId &nodeId, NodeIdMap &m) {
    m.put(nodeId);
    return browseChildren(nodeId, m);
}

/*!
    \brief Open62541::Client::getEndpoints
    \param serverUrl
    \param list
    \return
*/
UA_StatusCode Open62541::Client::getEndpoints(const std::string &serverUrl, std::vector<std::string> &list) {
    if (_client) {
        UA_EndpointDescription *endpointDescriptions = nullptr;
        size_t endpointDescriptionsSize = 0;

        {
            WriteLock l(_mutex);
            _lastError = UA_Client_getEndpoints(_client, serverUrl.c_str(), &endpointDescriptionsSize, &endpointDescriptions);
        }
        if (_lastError == UA_STATUSCODE_GOOD) {
            for (int i = 0; i < int(endpointDescriptionsSize); i++) {

                list.push_back(toString(endpointDescriptions[i].endpointUrl));
            }
        }
        return _lastError;
    }
    throw std::runtime_error("Null client");
    return 0;
}


/*!
    \brief NodeIdFromPath
    \param path
    \param nameSpaceIndex
    \param nodeId
    \return
*/
bool Open62541::Client::nodeIdFromPath(NodeId &start, Path &path, NodeId &nodeId) {
    // nodeId is a shallow copy - do not delete and is volatile
    UA_NodeId n = start.get();

    int level = 0;
    if (path.size() > 0) {
        Open62541::ClientBrowser b(*this);
        while (level < int(path.size())) {
            b.browse(n);
            int i = b.find(path[level]);
            if (i < 0) return false;
            level++;
            n = (b.list()[i]).childId;
        }
    }

    nodeId = n; // deep copy
    return level == int(path.size());
}



/*!
    \brief createPath
    \param start
    \param path
    \param nameSpaceIndex
    \param nodeId
    \return
*/
bool Open62541::Client::createFolderPath(NodeId &start, Path &path, int nameSpaceIndex, NodeId &nodeId) {
    //
    // create folder path first then add varaibles to path's end leaf
    //
    UA_NodeId n = start.get();
    //
    int level = 0;
    if (path.size() > 0) {
        Open62541::ClientBrowser b(*this);
        while (level < int(path.size())) {
            b.browse(n);
            int i = b.find(path[level]);
            if (i < 0)  break;
            level++;
            n = (b.list()[i]).childId; // shallow copy
        }
        if (level == int(path.size())) {
            nodeId = n;
        }
        else {
            NodeId nf(nameSpaceIndex, 0); // auto generate NODE id
            nodeId = n;
            NodeId newNode;
            while (level < int(path.size())) {
                addFolder(nodeId, path[level], nf, newNode.notNull(), nameSpaceIndex);
                if (!lastOK()) {
                    break;
                }
                nodeId = newNode; // assign
                level++;
            }
        }
    }
    return level == int(path.size());
}

/*!
    \brief getChild
    \param nameSpaceIndex
    \param childName
    \return
*/
bool Open62541::Client::getChild(NodeId &start, const std::string &childName, NodeId &ret) {
    Path p;
    p.push_back(childName);
    return nodeIdFromPath(start, p, ret);
}

/*!
    \brief Open62541::Client::addFolder
    \param parent
    \param nameSpaceIndex
    \param childName
    \return
*/
bool Open62541::Client::addFolder(NodeId &parent,  const std::string &childName,
                                  NodeId &nodeId,  NodeId &newNode, int nameSpaceIndex) {
    WriteLock l(_mutex);
    //
    if (nameSpaceIndex == 0) nameSpaceIndex = parent.nameSpaceIndex(); // inherit parent by default
    //
    QualifiedName qn(nameSpaceIndex, childName);
    ObjectAttributes attr;
    attr.setDisplayName(childName);
    attr.setDescription(childName);
    //
    _lastError = UA_Client_addObjectNode(_client,
                                         nodeId,
                                         parent,
                                         NodeId::Organizes,
                                         qn,
                                         NodeId::FolderType,
                                         attr.get(),
                                         newNode.isNull()?nullptr:newNode.ref());

    return lastOK();
}

/*!
    \brief Open62541::Client::addFolder::addVariable
    \param parent
    \param nameSpaceIndex
    \param childName
    \return
*/
bool Open62541::Client::addVariable(NodeId &parent, const std::string &childName, Variant &value,
                                    NodeId &nodeId, NodeId &newNode, int nameSpaceIndex) {
    WriteLock l(_mutex);
    if (nameSpaceIndex == 0) nameSpaceIndex = parent.nameSpaceIndex(); // inherit parent by default
    VariableAttributes var_attr;
    QualifiedName qn(nameSpaceIndex, childName);
    var_attr.setDisplayName(childName);
    var_attr.setDescription(childName);
    var_attr.setValue(value);
    _lastError = UA_Client_addVariableNode(_client,
                                           nodeId, // Assign new/random NodeID
                                           parent,
                                           NodeId::Organizes,
                                           qn,
                                           NodeId::Null, // no variable type
                                           var_attr,
                                           newNode.isNull()?nullptr:newNode.ref());


    return lastOK();
}



