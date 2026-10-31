import QtQuick
QtObject {
	enum AccessType { User_AccessType_User = 0, User_AccessType_Installer = 1, User_AccessType_SuperUser = 2 }
	enum NotificationType { Notification_Warning = 0, Notification_Alarm = 1, Notification_Info = 2 }
	enum DoneOptions { ModalDialog_DoneOptions_NoOptions = 0, ModalDialog_DoneOptions_CancelOnly = 1, ModalDialog_DoneOptions_OkOnly = 2, ModalDialog_DoneOptions_OkAndCancel = 3, ModalDialog_DoneOptions_SetAndCancel = 4 }
	enum UnitsType { Units_None = 0, Units_Temperature_Celsius = 1, Units_Temperature_Fahrenheit = 2 }
	enum RoundedSide { AsymmetricRoundedRectangle_RoundedSide_All = 0, AsymmetricRoundedRectangle_RoundedSide_Left = 1, AsymmetricRoundedRectangle_RoundedSide_Right = 2, AsymmetricRoundedRectangle_RoundedSide_Top = 3, AsymmetricRoundedRectangle_RoundedSide_Bottom = 4, AsymmetricRoundedRectangle_RoundedSide_NoneHorizontal = 5 }
}
