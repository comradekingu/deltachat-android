/*******************************************************************************
 *
 *                              Delta Chat Android
 *                           (C) 2017 Björn Petersen
 *                    Contact: r10s@b44t.com, http://b44t.com
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see http://www.gnu.org/licenses/ .
 *
 *******************************************************************************
 *
 * File:    mr_wrapper.c
 * Purpose: The C part of the Java<->C Wrapper, see also MrMailbox.java
 *
 ******************************************************************************/


#include <jni.h>
#include <android/log.h>
#include "messenger-backend/src/mrmailbox.h"


#define CHAR_REF(a) \
	const char* a##Ptr = (a)? (*env)->GetStringUTFChars(env, (a), 0) : NULL; /* passing a NULL-jstring results in a NULL-ptr - this is needed eg. for mrchat_save_draft() and many others */

#define CHAR_UNREF(a) \
	if(a) { (*env)->ReleaseStringUTFChars(env, (a), a##Ptr); }

#define JSTRING_NEW(a) jstring_new__(env, (a))
static jstring jstring_new__(JNIEnv* env, const char* a)
{
	if( a==NULL || a[0]==0 ) {
		return (*env)->NewStringUTF(env, "");
	}

	/* for non-empty strings, do not use NewStringUTF() as this is buggy on some Android versions.
	Instead, create the string using `new String(ByteArray, "UTF-8);` which seems to be programmed more properly.
	(eg. on KitKat a simple "SMILING FACE WITH SMILING EYES" (U+1F60A, UTF-8 F0 9F 98 8A) will let the app crash, reporting 0xF0 is a bad UTF-8 start, 
	see http://stackoverflow.com/questions/12127817/android-ics-4-0-ndk-newstringutf-is-crashing-down-the-app ) */
	static jclass    s_strCls    = NULL;
	static jmethodID s_strCtor   = NULL;
	static jclass    s_strEncode = NULL;
	if( s_strCls==NULL ) {
		s_strCls    = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "java/lang/String"));
		s_strCtor   = (*env)->GetMethodID(env, s_strCls, "<init>", "([BLjava/lang/String;)V");
		s_strEncode = (*env)->NewGlobalRef(env, (*env)->NewStringUTF(env, "UTF-8"));
	}

	int a_bytes = strlen(a);
	jbyteArray array = (*env)->NewByteArray(env, a_bytes);
		(*env)->SetByteArrayRegion(env, array, 0, a_bytes, a);
		jstring ret = (jstring) (*env)->NewObject(env, s_strCls, s_strCtor, array, s_strEncode);
	(*env)->DeleteLocalRef(env, array); /* we have to delete the reference as it is not returned to Java, AFAIK */

	return ret;
}


/* global stuff */

static JavaVM*   s_jvm = NULL;
static jclass    s_MrMailbox_class = NULL;
static jmethodID s_MrCallback_methodID = NULL;
static int       s_global_init_done = 0;


static void s_init_globals(JNIEnv *env, jclass MrMailbox_class)
{
	/* make sure, the intialisation is done only once */
	if( s_global_init_done ) { return; }
	s_global_init_done = 1;
	
	/* prepare calling back a Java function */
	__android_log_print(ANDROID_LOG_INFO, "DeltaChat", "JNI: s_init_globals()..."); /* low-level logging, mrmailbox_log_*() may not be yet available. However, please note that __android_log_print() may not work (eg. on LG X Cam) */

	(*env)->GetJavaVM(env, &s_jvm); /* JNIEnv cannot be shared between threads, so we share the JavaVM object */
	s_MrMailbox_class =  (*env)->NewGlobalRef(env, MrMailbox_class);
	s_MrCallback_methodID = (*env)->GetStaticMethodID(env, MrMailbox_class, "MrCallback","(IJJ)J" /*signature as "(param)ret" with I=int, J=long*/ );
}


/* setup threads, only called directly by the backed */

int mrosnative_setup_thread(mrmailbox_t* mailbox)
{
	if( s_jvm == NULL ) {
		mrmailbox_log_error(mailbox, 0, "Not ready, cannot setup thread.");
		return 0;
	}

	mrmailbox_log_info(mailbox, 0, "Attaching C-thread to Java VM...");
		JNIEnv* env = NULL;
		(*s_jvm)->AttachCurrentThread(s_jvm, &env, NULL);
	mrmailbox_log_info(mailbox, 0, "Attaching ok.");
	return 1;
}


void mrosnative_unsetup_thread(mrmailbox_t* mailbox)
{
	mrmailbox_log_info(mailbox, 0, "Detaching C-thread from Java VM...");
		(*s_jvm)->DetachCurrentThread(s_jvm);
	mrmailbox_log_info(mailbox, 0, "DeltaChat", "Detaching done.");
}


/* tools */

static jintArray carray2jintArray_n_carray_free(JNIEnv *env, const carray* ca)
{
	int i, icnt = ca? carray_count(ca) : 0;
	jintArray ret = (*env)->NewIntArray(env, icnt); if (ret == NULL) { return NULL; }
	
	if( ca ) {
		if( icnt ) {
			void** ca_data = carray_data(ca);
			if( sizeof(void*)==sizeof(jint) ) {
				(*env)->SetIntArrayRegion(env, ret, 0, icnt, (jint*)ca_data);
			}
			else {
				jint* temp = calloc(icnt, sizeof(jint));
					for( i = 0; i < icnt; i++ ) {
						temp[i] = (jint)ca_data[i];
					}
					(*env)->SetIntArrayRegion(env, ret, 0, icnt, temp);
				free(temp);
			}
		}
		carray_free(ca);
	}

	return ret;
}


static uint32_t* jintArray2uint32Pointer(JNIEnv* env, jintArray ja, int* ret_icnt)
{
	uint32_t* ret = NULL;
	if( env && ja && ret_icnt )
	{
		int i, icnt  = (*env)->GetArrayLength(env, ja);
		if( icnt > 0 )
		{
			const jint* temp = (*env)->GetIntArrayElements(env, ja, NULL);
			if( temp )
			{
				ret = calloc(icnt, sizeof(uint32_t));
				if( ret )
				{
					for( i = 0; i < icnt; i++ ) {
						ret[i] = (uint32_t)temp[i];
					}
					*ret_icnt = icnt;
				}
				(*env)->ReleaseIntArrayElements(env, ja, temp, 0);
			}
		}
	}
	return ret;
}


/*******************************************************************************
 * MrMailbox
 ******************************************************************************/


static mrmailbox_t* get_mrmailbox_t(JNIEnv *env, jclass cls)
{
	static jfieldID fid = 0;
	if( fid == 0 ) {
		fid = (*env)->GetStaticFieldID(env, cls, "m_hMailbox", "J" /*Signature, J=long*/);
	}
	if( fid ) {
		return (mrmailbox_t*)(*env)->GetStaticLongField(env, cls, fid);
	}
	return NULL;
}
 

/* MrMailbox - new/delete */

static uintptr_t s_mailbox_callback_(mrmailbox_t* mailbox, int event, uintptr_t data1, uintptr_t data2)
{
	jlong   l;
	JNIEnv* env;

	#if 0 /* -- __android_log_print() does not log eg. on LG X Cam - but Javas Log.i() etc. do. So, we do not optimize these calls and just use the Java logging. */ 
	if( event==MR_EVENT_INFO || event==MR_EVENT_WARNING ) {
	    __android_log_print(event==MR_EVENT_INFO? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "DeltaChat", "%s", (char*)data2); /* on problems, add `-llog` to `Android.mk` */
		return 0; /* speed up things for info/warning */
	}
	else if( event == MR_EVENT_ERROR ) {
	    __android_log_print(ANDROID_LOG_ERROR, "DeltaChat", "%s", (char*)data2);
	    /* errors are also forwarded to Java to show them in a bubble or so */
	}
	#endif

	if( s_jvm==NULL || s_MrMailbox_class==NULL || s_MrCallback_methodID==NULL ) {
		return 0; /* may happen on startup */
	}

	(*s_jvm)->GetEnv(s_jvm, &env, JNI_VERSION_1_6); /* as this function may be called from _any_ thread, we cannot use a static pointer to JNIEnv */
	if( env==NULL ) {
		return 0; /* may happen on startup */
	}

	l = (*env)->CallStaticLongMethod(env, s_MrMailbox_class, s_MrCallback_methodID, (jint)event, (jlong)data1, (jlong)data2);
	return (uintptr_t)l;
}


JNIEXPORT jlong Java_com_b44t_messenger_MrMailbox_MrMailboxNew(JNIEnv *env, jclass c)
{
	s_init_globals(env, c);
	return (jlong)mrmailbox_new(s_mailbox_callback_, NULL);
}


/* MrMailbox - open/configure/connect/fetch */

JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_open(JNIEnv *env, jclass cls, jstring dbfile)
{
	CHAR_REF(dbfile);
		jint ret = mrmailbox_open(get_mrmailbox_t(env, cls), dbfilePtr, NULL);
	CHAR_UNREF(dbfile)
	return ret;
}


JNIEXPORT void Java_com_b44t_messenger_MrMailbox_close(JNIEnv *env, jclass cls)
{
	mrmailbox_close(get_mrmailbox_t(env, cls));
}


JNIEXPORT jstring Java_com_b44t_messenger_MrMailbox_getBlobdir(JNIEnv *env, jclass cls)
{
	mrmailbox_t* ths = get_mrmailbox_t(env, cls);
	return JSTRING_NEW((ths&&ths->m_blobdir)? ths->m_blobdir : NULL);
}


JNIEXPORT void Java_com_b44t_messenger_MrMailbox_configureAndConnect(JNIEnv *env, jclass cls)
{
	mrmailbox_configure_and_connect(get_mrmailbox_t(env, cls));
}


JNIEXPORT void Java_com_b44t_messenger_MrMailbox_configureCancel(JNIEnv *env, jclass cls)
{
	mrmailbox_configure_cancel(get_mrmailbox_t(env, cls));
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_isConfigured(JNIEnv *env, jclass cls)
{
	return (jint)mrmailbox_is_configured(get_mrmailbox_t(env, cls));
}


JNIEXPORT void Java_com_b44t_messenger_MrMailbox_connect(JNIEnv *env, jclass cls)
{
	mrmailbox_connect(get_mrmailbox_t(env, cls));
}


JNIEXPORT void Java_com_b44t_messenger_MrMailbox_disconnect(JNIEnv *env, jclass cls)
{
	mrmailbox_disconnect(get_mrmailbox_t(env, cls));
}


/* MrMailbox - handle contacts */

JNIEXPORT jintArray Java_com_b44t_messenger_MrMailbox_getKnownContacts(JNIEnv *env, jclass cls, jstring query)
{
	CHAR_REF(query);
	    carray* ca = mrmailbox_get_known_contacts(get_mrmailbox_t(env, cls), queryPtr);
	CHAR_UNREF(query);
	return carray2jintArray_n_carray_free(env, ca);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_getBlockedCount(JNIEnv *env, jclass cls)
{
	return mrmailbox_get_blocked_count(get_mrmailbox_t(env, cls));
}


JNIEXPORT jintArray Java_com_b44t_messenger_MrMailbox_getBlockedContacts(JNIEnv *env, jclass cls)
{
	carray* ca = mrmailbox_get_blocked_contacts(get_mrmailbox_t(env, cls));
	return carray2jintArray_n_carray_free(env, ca);
}


JNIEXPORT jlong Java_com_b44t_messenger_MrMailbox_MrMailboxGetContact(JNIEnv *env, jclass c, jlong hMailbox, jint contact_id)
{
	return (jlong)mrmailbox_get_contact((mrmailbox_t*)hMailbox, contact_id);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_MrMailboxCreateContact(JNIEnv *env, jclass c, jlong hMailbox, jstring name, jstring addr)
{
	CHAR_REF(name);
	CHAR_REF(addr);
		jint ret = (jint)mrmailbox_create_contact((mrmailbox_t*)hMailbox, namePtr, addrPtr);
	CHAR_UNREF(addr);
	CHAR_UNREF(name);
	return ret;
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_MrMailboxBlockContact(JNIEnv *env, jclass c, jlong hMailbox, jint contact_id, jint block)
{
	return (jint)mrmailbox_block_contact((mrmailbox_t*)hMailbox, contact_id, block);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_MrMailboxDeleteContact(JNIEnv *env, jclass c, jlong hMailbox, jint contact_id)
{
	return (jint)mrmailbox_delete_contact((mrmailbox_t*)hMailbox, contact_id);
}


/* MrMailbox - handle chats */

JNIEXPORT jlong Java_com_b44t_messenger_MrMailbox_MrMailboxGetChatlist(JNIEnv *env, jclass c, jlong hMailbox, jstring query)
{
	jlong ret;
	if( query ) {
		CHAR_REF(query);
			ret = (jlong)mrmailbox_get_chatlist((mrmailbox_t*)hMailbox, queryPtr);
		CHAR_UNREF(query);
	}
	else {
		ret = (jlong)mrmailbox_get_chatlist((mrmailbox_t*)hMailbox, NULL);
	}
	return ret;
}


JNIEXPORT jlong Java_com_b44t_messenger_MrMailbox_MrMailboxGetChat(JNIEnv *env, jclass c, jlong hMailbox, jint chat_id)
{
	return (jlong)mrmailbox_get_chat((mrmailbox_t*)hMailbox, chat_id);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_getChatIdByContactId(JNIEnv *env, jclass cls, jint contact_id)
{
	return (jint)mrmailbox_get_chat_id_by_contact_id(get_mrmailbox_t(env, cls), contact_id);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_markseenMsgs(JNIEnv *env, jclass cls, jintArray msg_ids)
{
	int msg_ids_cnt;
	const uint32_t* msg_ids_ptr = jintArray2uint32Pointer(env, msg_ids, &msg_ids_cnt);
		jint ret = mrmailbox_markseen_msgs(get_mrmailbox_t(env, cls), msg_ids_ptr, msg_ids_cnt);
	free(msg_ids_ptr);
	return ret;
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_marknoticedChat(JNIEnv *env, jclass cls, jint chat_id)
{
	return (jlong)mrmailbox_marknoticed_chat(get_mrmailbox_t(env, cls), chat_id);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_createChatByContactId(JNIEnv *env, jclass cls, jint contact_id)
{
	return (jint)mrmailbox_create_chat_by_contact_id(get_mrmailbox_t(env, cls), contact_id);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_createGroupChat(JNIEnv *env, jclass cls, jstring name)
{
	CHAR_REF(name);
		jint ret = (jint)mrmailbox_create_group_chat(get_mrmailbox_t(env, cls), namePtr);
	CHAR_UNREF(name);
	return ret;
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_isContactInChat(JNIEnv *env, jclass cls, jint chat_id, jint contact_id)
{
	return (jint)mrmailbox_is_contact_in_chat(get_mrmailbox_t(env, cls), chat_id, contact_id);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_addContactToChat(JNIEnv *env, jclass cls, jint chat_id, jint contact_id)
{
	return (jint)mrmailbox_add_contact_to_chat(get_mrmailbox_t(env, cls), chat_id, contact_id);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_removeContactFromChat(JNIEnv *env, jclass cls, jint chat_id, jint contact_id)
{
	return (jint)mrmailbox_remove_contact_from_chat(get_mrmailbox_t(env, cls), chat_id, contact_id);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_setChatName(JNIEnv *env, jclass cls, jint chat_id, jstring name)
{
	CHAR_REF(name);
		jint ret = (jint)mrmailbox_set_chat_name(get_mrmailbox_t(env, cls), chat_id, namePtr);
	CHAR_UNREF(name);
	return ret;
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_setChatImage(JNIEnv *env, jclass cls, jint chat_id, jstring image/*NULL=delete*/)
{
	CHAR_REF(image);
	jint ret = (jint)mrmailbox_set_chat_image(get_mrmailbox_t(env, cls), chat_id, imagePtr/*CHAR_REF() preserves NULL*/);
	CHAR_UNREF(image);
	return ret;
}


JNIEXPORT void Java_com_b44t_messenger_MrMailbox_deleteChat(JNIEnv *env, jclass cls, jint chat_id)
{
	mrmailbox_delete_chat(get_mrmailbox_t(env, cls), chat_id);
}


/* MrMailbox - handle messages */

JNIEXPORT jlong Java_com_b44t_messenger_MrMailbox_MrMailboxGetMsg(JNIEnv *env, jclass c, jlong hMailbox, jint id)
{
	return (jlong)mrmailbox_get_msg((mrmailbox_t*)hMailbox, id);
}


JNIEXPORT jstring Java_com_b44t_messenger_MrMailbox_MrMailboxGetMsgInfo(JNIEnv *env, jclass c, jlong hMailbox, jint msg_id)
{
	char* temp = mrmailbox_get_msg_info((mrmailbox_t*)hMailbox, msg_id);
		jstring ret = JSTRING_NEW(temp);
	free(temp);
	return ret;
}


JNIEXPORT void Java_com_b44t_messenger_MrMailbox_deleteMsgs(JNIEnv *env, jclass cls, jintArray msg_ids)
{
	int msg_ids_cnt;
	const uint32_t* msg_ids_ptr = jintArray2uint32Pointer(env, msg_ids, &msg_ids_cnt);
		mrmailbox_delete_msgs(get_mrmailbox_t(env, cls), msg_ids_ptr, msg_ids_cnt);
	free(msg_ids_ptr);
}


JNIEXPORT void Java_com_b44t_messenger_MrMailbox_forwardMsgs(JNIEnv *env, jclass cls, jintArray msg_ids, jint chat_id)
{
	int msg_ids_cnt;
	const uint32_t* msg_ids_ptr = jintArray2uint32Pointer(env, msg_ids, &msg_ids_cnt);
		mrmailbox_forward_msgs(get_mrmailbox_t(env, cls), msg_ids_ptr, msg_ids_cnt, chat_id); 
	free(msg_ids_ptr);
}


/* MrMailbox - handle config */

JNIEXPORT void Java_com_b44t_messenger_MrMailbox_setConfig(JNIEnv *env, jclass cls, jstring key, jstring value /*may be NULL*/)
{
	CHAR_REF(key);
	CHAR_REF(value);
		mrmailbox_set_config(get_mrmailbox_t(env, cls), keyPtr, valuePtr /*is NULL if value is NULL, CHAR_REF() handles this*/);
	CHAR_UNREF(key);
	CHAR_UNREF(value);
}


JNIEXPORT void Java_com_b44t_messenger_MrMailbox_setConfigInt(JNIEnv *env, jclass cls, jstring key, jint value)
{
	CHAR_REF(key);
		mrmailbox_set_config_int(get_mrmailbox_t(env, cls), keyPtr, value);
	CHAR_UNREF(key);
}


JNIEXPORT jstring Java_com_b44t_messenger_MrMailbox_getConfig(JNIEnv *env, jclass cls, jstring key, jstring def/*may be NULL*/)
{
	CHAR_REF(key);
	CHAR_REF(def);
		char* temp = mrmailbox_get_config(get_mrmailbox_t(env, cls), keyPtr, defPtr /*is NULL if value is NULL, CHAR_REF() handles this*/);
			jstring ret = NULL;
			if( temp ) {
				ret = JSTRING_NEW(temp);
			}
		free(temp);
	CHAR_UNREF(key);
	CHAR_UNREF(def);
	return ret; /* returns NULL only if key is unset and "def" is NULL */
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_getConfigInt(JNIEnv *env, jclass cls, jstring key, jint def)
{
	CHAR_REF(key);
		jint ret = mrmailbox_get_config_int(get_mrmailbox_t(env, cls), keyPtr, def);
	CHAR_UNREF(key);
	return ret;
}


/* MrMailbox - misc. */

JNIEXPORT jstring Java_com_b44t_messenger_MrMailbox_getInfo(JNIEnv *env, jclass cls)
{
	char* temp = mrmailbox_get_info(get_mrmailbox_t(env, cls));
		jstring ret = JSTRING_NEW(temp);
	free(temp);
	return ret;
}


JNIEXPORT jstring Java_com_b44t_messenger_MrMailbox_getContactEncrInfo(JNIEnv *env, jclass cls, jint contact_id)
{
	char* temp = mrmailbox_get_contact_encrinfo(get_mrmailbox_t(env, cls), contact_id);
		jstring ret = JSTRING_NEW(temp);
	free(temp);
	return ret;
}


JNIEXPORT jstring Java_com_b44t_messenger_MrMailbox_cmdline(JNIEnv *env, jclass cls, jstring cmd)
{
	CHAR_REF(cmd);
		char* temp = mrmailbox_cmdline(get_mrmailbox_t(env, cls), cmdPtr);
			jstring ret = JSTRING_NEW(temp);
		free(temp);
	CHAR_UNREF(cmd);
	return ret;
}


JNIEXPORT void Java_com_b44t_messenger_MrMailbox_imex(JNIEnv *env, jclass cls, jint what, jstring dir)
{
	CHAR_REF(dir);
		mrmailbox_imex(get_mrmailbox_t(env, cls), what, dirPtr, "");
	CHAR_UNREF(dir);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_checkPassword(JNIEnv *env, jclass cls, jstring pw)
{
	CHAR_REF(pw);
		jint r = mrmailbox_check_password(get_mrmailbox_t(env, cls),  pwPtr);
	CHAR_UNREF(pw);
	return r;
}


JNIEXPORT jstring Java_com_b44t_messenger_MrMailbox_imexHasBackup(JNIEnv *env, jclass cls, jstring dir)
{
	CHAR_REF(dir);
		jstring ret = NULL;
		char* temp = mrmailbox_imex_has_backup(get_mrmailbox_t(env, cls),  dirPtr);
		if( temp ) {
			ret = JSTRING_NEW(temp);
			free(temp);
		}
	CHAR_UNREF(dir);
	return ret; /* may be NULL! */
}


JNIEXPORT void Java_com_b44t_messenger_MrMailbox_heartbeat(JNIEnv *env, jclass cls)
{
    mrmailbox_heartbeat(get_mrmailbox_t(env, cls));
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_MrMailboxAddAddressBook(JNIEnv *env, jclass c, jlong hMailbox, jstring adrbook)
{
	CHAR_REF(adrbook);
		int modify_count = mrmailbox_add_address_book((mrmailbox_t*)hMailbox, adrbookPtr);
	CHAR_UNREF(adrbook);
	return modify_count;
}



/*******************************************************************************
 * MrChatlist
 ******************************************************************************/


JNIEXPORT void Java_com_b44t_messenger_MrChatlist_MrChatlistUnref(JNIEnv *env, jclass c, jlong hChatlist)
{
	mrchatlist_unref((mrchatlist_t*)hChatlist);
}


JNIEXPORT jint Java_com_b44t_messenger_MrChatlist_MrChatlistGetCnt(JNIEnv *env, jclass c, jlong hChatlist)
{
	return mrchatlist_get_cnt((mrchatlist_t*)hChatlist);
}


JNIEXPORT jlong Java_com_b44t_messenger_MrChatlist_MrChatlistGetChatByIndex(JNIEnv *env, jclass c, jlong hChatlist, jint index)
{
	return (jlong)mrchatlist_get_chat_by_index((mrchatlist_t*)hChatlist, index);
}


JNIEXPORT jlong Java_com_b44t_messenger_MrChatlist_MrChatlistGetSummaryByIndex(JNIEnv *env, jclass c, jlong hChatlist, jint index, jlong hChat)
{
	return (jlong)mrchatlist_get_summary_by_index((mrchatlist_t*)hChatlist, index, (mrchat_t*)hChat);
}


/*******************************************************************************
 * MrChat
 ******************************************************************************/


static mrchat_t* get_mrchat_t(JNIEnv *env, jobject obj)
{
	static jfieldID fid = 0;
	if( fid == 0 ) {
		jclass cls = (*env)->GetObjectClass(env, obj);
		fid = (*env)->GetFieldID(env, cls, "m_hChat", "J" /*Signature, J=long*/);
	}
	if( fid ) {
		return (mrchat_t*)(*env)->GetLongField(env, obj, fid);
	}
	return NULL;
}


JNIEXPORT void Java_com_b44t_messenger_MrChat_MrChatUnref(JNIEnv *env, jclass c, jlong hChat)
{
	mrchat_unref((mrchat_t*)hChat);
}


JNIEXPORT jint Java_com_b44t_messenger_MrChat_getId(JNIEnv *env, jclass cls)
{
	mrchat_t* ths = get_mrchat_t(env, cls); if( ths == NULL ) { return 0; }
	return ths->m_id;
}


JNIEXPORT jint Java_com_b44t_messenger_MrChat_getType(JNIEnv *env, jclass cls)
{
	mrchat_t* ths = get_mrchat_t(env, cls); if( ths == NULL ) { return 0; }
	return ths->m_type;
}


JNIEXPORT jstring Java_com_b44t_messenger_MrChat_getName(JNIEnv *env, jclass cls)
{
	mrchat_t* ths = get_mrchat_t(env, cls); if( ths == NULL ) { return JSTRING_NEW(NULL); }
	return JSTRING_NEW(ths->m_name);
}


JNIEXPORT jstring Java_com_b44t_messenger_MrChat_getSubtitle(JNIEnv *env, jclass cls)
{
	const char* temp = mrchat_get_subtitle(get_mrchat_t(env, cls));
		jstring ret = JSTRING_NEW(temp);
	free(temp);
	return ret;
}


JNIEXPORT jint Java_com_b44t_messenger_MrChat_getParam(JNIEnv *env, jclass cls, jint key, jstring def)
{
    mrchat_t* ths = get_mrchat_t(env, cls);
    jstring ret = NULL;
    CHAR_REF(def);
        char* temp = mrparam_get(ths? ths->m_param:NULL, key, defPtr);
        if( temp ) {
            ret = JSTRING_NEW(temp);
            free(temp);
        }
    CHAR_UNREF(def);
    return ret; /* returns NULL only if key is unset and "def" is NULL */
}


JNIEXPORT jint Java_com_b44t_messenger_MrChat_getParamInt(JNIEnv *env, jclass cls, jint key, jint def)
{
	mrchat_t* ths = get_mrchat_t(env, cls);
	return mrparam_get_int(ths? ths->m_param:NULL, key, def);
}


JNIEXPORT jstring Java_com_b44t_messenger_MrChat_MrChatGetDraft(JNIEnv *env, jclass c, jlong hChat) /* returns NULL for "no draft" */
{
	mrchat_t* ths = (mrchat_t*)hChat;
	if( ths && ths->m_draft_text ) {
		return JSTRING_NEW(ths->m_draft_text);
	}
	return NULL; /* no draft */
}


JNIEXPORT jlong Java_com_b44t_messenger_MrChat_MrChatGetDraftTimestamp(JNIEnv *env, jclass c, jlong hChat)
{
	mrchat_t* ths = (mrchat_t*)hChat; if( ths == NULL ) { return 0; }
	return ths->m_draft_timestamp;
}


JNIEXPORT jint Java_com_b44t_messenger_MrChat_MrChatGetDraftReplyToMsgId(JNIEnv *env, jclass c, jlong hChat)
{
	mrchat_t* ths = (mrchat_t*)hChat; if( ths == NULL ) { return 0; }
	return 0;
}


JNIEXPORT jint Java_com_b44t_messenger_MrChat_getFreshMsgCount(JNIEnv *env, jclass cls)
{
	return mrchat_get_fresh_msg_count(get_mrchat_t(env, cls));
}


JNIEXPORT jint Java_com_b44t_messenger_MrChat_MrChatSetDraft(JNIEnv *env, jclass c, jlong hChat, jstring draft /* NULL=delete */, jint replyToMsgId)
{
	CHAR_REF(draft);
		jint ret = (jint)mrchat_set_draft((mrchat_t*)hChat, draftPtr /* NULL=delete */);
	CHAR_UNREF(draft);
	return ret;
}


JNIEXPORT jint Java_com_b44t_messenger_MrChat_sendText(JNIEnv *env, jclass cls, jstring text)
{
	mrmsg_t* msg = mrmsg_new();
		msg->m_type = MR_MSG_TEXT;
		CHAR_REF(text);
			msg->m_text = textPtr? strdup(textPtr) : NULL;
		CHAR_UNREF(text);
		jint msg_id = mrchat_send_msg(get_mrchat_t(env, cls), msg);
	mrmsg_unref(msg);
	return msg_id;
}


JNIEXPORT jint Java_com_b44t_messenger_MrChat_sendMedia(JNIEnv *env, jclass cls, jint type, jstring file, jstring mime, jint w, jint h, jint ms, jstring author, jstring trackname)
{
	mrmsg_t* msg = mrmsg_new();
		msg->m_type = type;
		CHAR_REF(mime);
			mrparam_set(msg->m_param, 'm', mimePtr);
		CHAR_UNREF(mime);
		CHAR_REF(file);
			mrparam_set(msg->m_param, 'f', filePtr);
		CHAR_UNREF(file);		
		if( type == MR_MSG_IMAGE || type == MR_MSG_GIF || type == MR_MSG_VIDEO ) {
			mrparam_set_int(msg->m_param, 'w', w);
			mrparam_set_int(msg->m_param, 'h', h);
		}
		if( type == MR_MSG_AUDIO || type == MR_MSG_VOICE || type == MR_MSG_VIDEO ) {
			mrparam_set_int(msg->m_param, 'd', ms);
		}
		if( type == MR_MSG_AUDIO ) {
			CHAR_REF(author);
				mrparam_set(msg->m_param, 'N', authorPtr);
			CHAR_UNREF(author);
			CHAR_REF(trackname);
				mrparam_set(msg->m_param, 'n', tracknamePtr);
			CHAR_UNREF(trackname);
		}
		jint msg_id = mrchat_send_msg(get_mrchat_t(env, cls), msg);
	mrmsg_unref(msg);
	return msg_id;
}


JNIEXPORT jintArray Java_com_b44t_messenger_MrMailbox_getChatMedia(JNIEnv *env, jclass cls, jint chat_id, jint msg_type, jint or_msg_type)
{
	carray* ca = mrmailbox_get_chat_media(get_mrmailbox_t(env, cls), chat_id, msg_type, or_msg_type);
	return carray2jintArray_n_carray_free(env, ca);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_getNextMedia(JNIEnv *env, jclass cls, jint msg_id, jint dir)
{
	return mrmailbox_get_next_media(get_mrmailbox_t(env, cls), msg_id, dir);
}


JNIEXPORT jintArray Java_com_b44t_messenger_MrMailbox_getChatMsgs(JNIEnv *env, jclass cls, jint chat_id, jint flags, jint marker1before)
{
	carray* ca = mrmailbox_get_chat_msgs(get_mrmailbox_t(env, cls), chat_id, flags, marker1before);
	return carray2jintArray_n_carray_free(env, ca);
}


JNIEXPORT jintArray Java_com_b44t_messenger_MrMailbox_searchMsgs(JNIEnv *env, jclass cls, jint chat_id, jstring query)
{
	CHAR_REF(query);
		carray* ca = mrmailbox_search_msgs(get_mrmailbox_t(env, cls), chat_id, queryPtr);
	CHAR_UNREF(query);
	return carray2jintArray_n_carray_free(env, ca);
}


JNIEXPORT jintArray Java_com_b44t_messenger_MrMailbox_getFreshMsgs(JNIEnv *env, jclass cls)
{
	carray* ca = mrmailbox_get_fresh_msgs(get_mrmailbox_t(env, cls));
	return carray2jintArray_n_carray_free(env, ca);
}


JNIEXPORT jintArray Java_com_b44t_messenger_MrMailbox_getChatContacts(JNIEnv *env, jclass cls, jint chat_id)
{
	carray* ca = mrmailbox_get_chat_contacts(get_mrmailbox_t(env, cls), chat_id);
	return carray2jintArray_n_carray_free(env, ca);
}


/*******************************************************************************
 * MrMsg
 ******************************************************************************/


static mrmsg_t* get_mrmsg_t(JNIEnv *env, jobject obj)
{
	static jfieldID fid = 0;
	if( fid == 0 ) {
		jclass cls = (*env)->GetObjectClass(env, obj);
		fid = (*env)->GetFieldID(env, cls, "m_hMsg", "J" /*Signature, J=long*/);
	}
	if( fid ) {
		return (mrmsg_t*)(*env)->GetLongField(env, obj, fid);
	}
	return NULL;
}


JNIEXPORT void Java_com_b44t_messenger_MrMsg_MrMsgUnref(JNIEnv *env, jclass c, jlong hMsg)
{
	mrmsg_unref((mrmsg_t*)hMsg);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMsg_MrMsgGetId(JNIEnv *env, jclass c, jlong hMsg)
{
	mrmsg_t* ths = (mrmsg_t*)hMsg; if( ths == NULL ) { return 0; }
	return ths->m_id;
}


JNIEXPORT jstring Java_com_b44t_messenger_MrMsg_MrMsgGetText(JNIEnv *env, jclass c, jlong hMsg)
{
	mrmsg_t* ths = (mrmsg_t*)hMsg; if( ths == NULL ) { return JSTRING_NEW(NULL); }
	return JSTRING_NEW(ths->m_text);
}


JNIEXPORT jlong Java_com_b44t_messenger_MrMsg_MrMsgGetTimestamp(JNIEnv *env, jclass c, jlong hMsg)
{
	mrmsg_t* ths = (mrmsg_t*)hMsg; if( ths == NULL ) { return 0; }
	return ths->m_timestamp;
}


JNIEXPORT jint Java_com_b44t_messenger_MrMsg_MrMsgGetType(JNIEnv *env, jclass c, jlong hMsg)
{
	mrmsg_t* ths = (mrmsg_t*)hMsg; if( ths == NULL ) { return 0; }
	return ths->m_type;
}


JNIEXPORT jint Java_com_b44t_messenger_MrMsg_MrMsgGetState(JNIEnv *env, jclass c, jlong hMsg)
{
	mrmsg_t* ths = (mrmsg_t*)hMsg; if( ths == NULL ) { return 0; }
	return ths->m_state;
}


JNIEXPORT jint Java_com_b44t_messenger_MrMsg_MrMsgGetChatId(JNIEnv *env, jclass c, jlong hMsg)
{
	mrmsg_t* ths = (mrmsg_t*)hMsg; if( ths == NULL ) { return 0; }
	return ths->m_chat_id;
}


JNIEXPORT jint Java_com_b44t_messenger_MrMsg_MrMsgGetFromId(JNIEnv *env, jclass c, jlong hMsg)
{
	mrmsg_t* ths = (mrmsg_t*)hMsg; if( ths == NULL ) { return 0; }
	return ths->m_from_id;
}


JNIEXPORT jint Java_com_b44t_messenger_MrMsg_MrMsgGetToId(JNIEnv *env, jclass c, jlong hMsg)
{
	mrmsg_t* ths = (mrmsg_t*)hMsg; if( ths == NULL ) { return 0; }
	return ths->m_to_id;
}


JNIEXPORT jstring Java_com_b44t_messenger_MrMsg_getParam(JNIEnv *env, jobject obj, jint key, jstring def)
{
	mrmsg_t* ths = get_mrmsg_t(env, obj);
	jstring ret = NULL;
	CHAR_REF(def);
		char* temp = mrparam_get(ths? ths->m_param:NULL, key, defPtr);
        if( temp ) {
            ret = JSTRING_NEW(temp);
            free(temp);
        }
	CHAR_UNREF(def);
	return ret;
}


JNIEXPORT jint Java_com_b44t_messenger_MrMsg_getParamInt(JNIEnv *env, jobject obj, jint key, jint def)
{
	mrmsg_t* ths = get_mrmsg_t(env, obj);
	return mrparam_get_int(ths? ths->m_param:NULL, key, def);
}


JNIEXPORT void Java_com_b44t_messenger_MrMsg_setParamInt(JNIEnv *env, jobject obj, jint key, jint value)
{
	mrmsg_t* ths = get_mrmsg_t(env, obj);
	mrparam_set_int(ths? ths->m_param:NULL, key, value);
}


JNIEXPORT void Java_com_b44t_messenger_MrMsg_saveParamToDisk(JNIEnv *env, jobject obj)
{
	mrmsg_save_param_to_disk(get_mrmsg_t(env, obj));
}


JNIEXPORT jint Java_com_b44t_messenger_MrMsg_getBytes(JNIEnv *env, jobject obj)
{
	jint ret = 0;
	mrmsg_t* ths = get_mrmsg_t(env, obj);
	if( ths ) {
		const char* file = mrparam_get(ths->m_param, 'f', NULL);
		if( file ) {
			ret = mr_get_filebytes(file);
			free(file);
		}
	}
	return ret;
}


JNIEXPORT jlong Java_com_b44t_messenger_MrMsg_getSummaryCPtr(JNIEnv *env, jobject obj, jlong hChat)
{
	return (jlong)mrmsg_get_summary(get_mrmsg_t(env, obj), (mrchat_t*)hChat);
}


JNIEXPORT jint Java_com_b44t_messenger_MrMsg_getSummarytext(JNIEnv *env, jobject obj, jint approx_characters)
{
	return JSTRING_NEW(mrmsg_get_summarytext(get_mrmsg_t(env, obj), approx_characters));
}


JNIEXPORT jint Java_com_b44t_messenger_MrMsg_showPadlock(JNIEnv *env, jobject obj)
{
	return mrmsg_show_padlock(get_mrmsg_t(env, obj));
}


JNIEXPORT jint Java_com_b44t_messenger_MrMsg_getFilename(JNIEnv *env, jobject obj)
{
	return JSTRING_NEW(mrmsg_get_filename(get_mrmsg_t(env, obj)));
}


JNIEXPORT jlong Java_com_b44t_messenger_MrMsg_getMediainfoCPtr(JNIEnv *env, jobject obj)
{
	return (jlong)mrmsg_get_mediainfo(get_mrmsg_t(env, obj));
}


JNIEXPORT jint Java_com_b44t_messenger_MrMsg_isIncreation(JNIEnv *env, jobject obj)
{
    return (jint)mrmsg_is_increation(get_mrmsg_t(env, obj));
}


/*******************************************************************************
 * MrContact
 ******************************************************************************/


static mrcontact_t* get_mrcontact_t(JNIEnv *env, jobject obj)
{
	static jfieldID fid = 0;
	if( fid == 0 ) {
		jclass cls = (*env)->GetObjectClass(env, obj);
		fid = (*env)->GetFieldID(env, cls, "m_hContact", "J" /*Signature, J=long*/);
	}
	if( fid ) {
		return (mrcontact_t*)(*env)->GetLongField(env, obj, fid);
	}
	return NULL;
}


JNIEXPORT void Java_com_b44t_messenger_MrContact_MrContactUnref(JNIEnv *env, jclass c, jlong hContact)
{
	mrcontact_unref((mrcontact_t*)hContact);
}


JNIEXPORT jstring Java_com_b44t_messenger_MrContact_MrContactGetName(JNIEnv *env, jclass c, jlong hContact)
{
	mrcontact_t* ths = (mrcontact_t*)hContact; if( ths == NULL ) { return JSTRING_NEW(NULL); }
	return JSTRING_NEW(ths->m_name);
}


JNIEXPORT jstring Java_com_b44t_messenger_MrContact_getAuthName(JNIEnv *env, jclass cls)
{
	mrcontact_t* ths = get_mrcontact_t(env, cls); if( ths == NULL ) { return JSTRING_NEW(NULL); }
	return JSTRING_NEW(ths->m_authname);
}


JNIEXPORT jstring Java_com_b44t_messenger_MrContact_MrContactGetAddr(JNIEnv *env, jclass c, jlong hContact)
{
	mrcontact_t* ths = (mrcontact_t*)hContact; if( ths == NULL ) { return JSTRING_NEW(NULL); }
	return JSTRING_NEW(ths->m_addr);
}


JNIEXPORT jint Java_com_b44t_messenger_MrContact_MrContactIsBlocked(JNIEnv *env, jclass c, jlong hContact)
{
	mrcontact_t* ths = (mrcontact_t*)hContact; if( ths == NULL ) { return 0; }
	return (jint)ths->m_blocked;
}


/*******************************************************************************
 * MrPoortext
 ******************************************************************************/


JNIEXPORT void Java_com_b44t_messenger_MrPoortext_MrPoortextUnref(JNIEnv *env, jclass c, jlong hPoortext)
{
	mrpoortext_unref((mrpoortext_t*)hPoortext);
}


JNIEXPORT jstring Java_com_b44t_messenger_MrPoortext_MrPoortextGetText1(JNIEnv *env, jclass c, jlong hPoortext)
{
	mrpoortext_t* ths = (mrpoortext_t*)hPoortext; if( ths == NULL ) { return JSTRING_NEW(NULL); }
	return JSTRING_NEW(ths->m_text1);
}


JNIEXPORT jint Java_com_b44t_messenger_MrPoortext_MrPoortextGetText1Meaning(JNIEnv *env, jclass c, jlong hPoortext)
{
	mrpoortext_t* ths = (mrpoortext_t*)hPoortext; if( ths == NULL ) { return 0; }
	return ths->m_text1_meaning;
}


JNIEXPORT jstring Java_com_b44t_messenger_MrPoortext_MrPoortextGetText2(JNIEnv *env, jclass c, jlong hPoortext)
{
	mrpoortext_t* ths = (mrpoortext_t*)hPoortext; if( ths == NULL ) { return JSTRING_NEW(NULL); }
	return JSTRING_NEW(ths->m_text2);
}


JNIEXPORT jlong Java_com_b44t_messenger_MrPoortext_MrPoortextGetTimestamp(JNIEnv *env, jclass c, jlong hPoortext)
{
	mrpoortext_t* ths = (mrpoortext_t*)hPoortext; if( ths == NULL ) { return 0; }
	return ths->m_timestamp;
}


JNIEXPORT jint Java_com_b44t_messenger_MrPoortext_MrPoortextGetState(JNIEnv *env, jclass c, jlong hPoortext)
{
	mrpoortext_t* ths = (mrpoortext_t*)hPoortext; if( ths == NULL ) { return 0; }
	return ths->m_state;
}


/*******************************************************************************
 * Tools
 ******************************************************************************/


JNIEXPORT jstring Java_com_b44t_messenger_MrMailbox_CPtr2String(JNIEnv *env, jclass c, jlong hStr)
{
	/* the callback may return a long that represents a pointer to a C-String; this function creates a Java-string from such values. */
	if( hStr == 0 ) {
		return NULL;
	}
	const char* ptr = (const char*)hStr;
	return JSTRING_NEW(ptr);
}


JNIEXPORT jlong Java_com_b44t_messenger_MrMailbox_String2CPtr(JNIEnv *env, jclass c, jstring str)
{
    char* hStr = NULL;
    if( str ) {
        CHAR_REF(str);
            hStr = strdup(strPtr);
        CHAR_UNREF(str);
    }
    return (jlong)hStr;
}


JNIEXPORT jstring Java_com_b44t_messenger_MrMailbox_MrGetVersionStr(JNIEnv *env, jclass c)
{
	s_init_globals(env, c);
	const char* temp = mrmailbox_get_version_str();
		jstring ret = JSTRING_NEW(temp);
	free(temp);
	return ret;
}


#include <time.h>
JNIEXPORT jlong Java_com_b44t_messenger_MrMailbox_getCurrentTimeMillis(JNIEnv *env, jclass c) {
	struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (jlong) ts.tv_sec * 1000 + (int64_t) ts.tv_nsec / 1000000;
}
JNIEXPORT jint Java_com_b44t_messenger_MrMailbox_getCurrentTime(JNIEnv *env, jclass c) {
    return (jint) (Java_com_b44t_messenger_MrMailbox_getCurrentTimeMillis(env, c) / 1000);
}


