import QtQuick
import QtQuick.Controls
import UConnect

// Lists the connected devices that offer one service; picking one opens that
// service's window for the device.
UMenu {
    id: menu

    required property string service
    property var devices: []

    signal openRequested(string devId)

    title: Theme.serviceLabel(service)

    // Built when shown rather than bound live, so a refresh cannot rebuild the
    // items under the pointer.
    onAboutToShow: devices = App.peers.devicesOffering(service)

    Instantiator {
        model: menu.devices
        delegate: UMenuItem {
            required property var modelData
            text: modelData.displayName
            hint: modelData.shortId
            onTriggered: menu.openRequested(modelData.devId)
        }
        onObjectAdded: (index, object) => menu.insertItem(index, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }

    UMenuItem {
        text: qsTr("No connected device offers this")
        enabled: false
        visible: menu.devices.length === 0
    }
}
