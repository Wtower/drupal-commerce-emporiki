<?php
/**
 * @file
 * Commerce Emporiki menu items.
 */

/**
 * Process result
 *
 * @see http://drupalcontrib.org/api/drupal/contributions%21commerce%21modules%21payment%21commerce_payment.module/function/commerce_payment_method_load/7
 */
function commerce_emporiki_process($result) {
  // Get the _POST data from alphabank.
  $data = array(
    'mid' => isset($_POST['mid'])? check_plain($_POST['mid']): '',
    'orderid' => isset($_POST['orderid'])? check_plain($_POST['orderid']): '',
    'status' => isset($_POST['status'])? check_plain($_POST['status']): '',
    'orderAmount' => isset($_POST['orderAmount'])? check_plain($_POST['orderAmount']): '',
    'currency' => isset($_POST['currency'])? check_plain($_POST['currency']): '',
    'paymentTotal' => isset($_POST['paymentTotal'])? check_plain($_POST['paymentTotal']): '',
    'message' => isset($_POST['message'])? check_plain($_POST['message']): '',
    'riskScore' => isset($_POST['riskScore'])? check_plain($_POST['riskScore']): '',
    'payMethod' => isset($_POST['payMethod'])? check_plain($_POST['payMethod']): '',
    'txId' => isset($_POST['txId'])? check_plain($_POST['txId']): '',
    'paymentRef' => isset($_POST['paymentRef'])? check_plain($_POST['paymentRef']): '',
  );
  // Load order
  // comes in form o6p1
  preg_match('/o(\d+)p(\d+)/', $data['orderid'], $matches);
  $order_id = 0;
  if (isset($matches[1])) $order_id = $matches[1];
  $order = commerce_order_load($order_id);
  if (!$order) {
    $data['error'] = 'Invalid order ID';
    commerce_emporiki_process_tx('error', NULL, $data);
    drupal_goto('cart');
    return;
  }
  // Check order status
  if ($order->status != 'checkout_payment') {
    $data['error'] = 'Order status not in payment';
    commerce_emporiki_process_tx('error', $order, $data);
    drupal_goto(commerce_checkout_order_uri($order));
    return;
  }
  // Check order amount
  $wrapper = entity_metadata_wrapper('commerce_order', $order);
  $total = commerce_line_items_total($wrapper->commerce_line_items);
  $charge = commerce_currency_amount_to_decimal($total['amount'],
    $total['currency_code']);
  if ($charge != $data['orderAmount']) {
    $data['error'] = 'Order amount from redirection is incorrect';
    commerce_emporiki_process_tx('error', $order, $data);
    commerce_payment_redirect_pane_previous_page($order);
    drupal_goto(commerce_checkout_order_uri($order));
    return;
  }
  // Check order currency
  if (variable_get('commerce_default_currency', 'EUR') != $data['currency']) {
    $data['error'] = 'Order currency from redirection is incorrect';
    commerce_emporiki_process_tx('error', $order, $data);
    commerce_payment_redirect_pane_previous_page($order);
    drupal_goto(commerce_checkout_order_uri($order));
    return;
  }
  // Load payment
  $payment =
    commerce_payment_method_instance_load($order->data['payment_method']);
  if (!$payment) {
    $data['error'] = 'Cannot load payment';
    commerce_emporiki_process_tx('error', $order, $data);
    commerce_payment_redirect_pane_previous_page($order);
    drupal_goto(commerce_checkout_order_uri($order));
    return;
  }
  // Check MID
  if ($payment['settings']['merchant_code'] != $data['mid']) {
    $data['error'] = 'Received wrong MID, possible forging attempt';
    commerce_emporiki_process_tx('error', $order, $data);
    commerce_payment_redirect_pane_previous_page($order);
    drupal_goto(commerce_checkout_order_uri($order));
    return;
  }
  // Check digest
  $data['secret'] = $payment['settings']['shared_secret_key'];
  $digest_post = isset($_POST['digest'])? check_plain($_POST['digest']): '';
  $digest = base64_encode(sha1(implode('', $data), TRUE));
  if ($digest_post != $digest) {
    $data['error'] = 'Received wrong digest, possible forging attempt';
    commerce_emporiki_process_tx('error', $order, $data);
    commerce_payment_redirect_pane_previous_page($order);
    drupal_goto(commerce_checkout_order_uri($order));
    return;
  }
  commerce_emporiki_process_tx($result, $order, $data);
  drupal_goto(commerce_checkout_order_uri($order));
}

function commerce_emporiki_process_tx($result, $order, $data) {
  if ($order) {
    $transaction = commerce_payment_transaction_new('commerce_emporiki', $order->order_id);
    $transaction->instance_id = $order->data['payment_method'];
    $transaction->amount = commerce_currency_decimal_to_amount(
      $data['paymentTotal'], $data['currency']);
    $transaction->currency_code = $data['currency'];
    $transaction->payload[REQUEST_TIME] = $_POST;
    $transaction->remote_id = $data['txId'];
    $transaction->remote_status = $data['status'];
    switch ($result) {
      case 'success':
        commerce_checkout_complete($order);
        commerce_payment_redirect_pane_next_page($order);
        switch ($data['status']) {
          case 'AUTHORIZED':
          case 'CAPTURED':
            $status = COMMERCE_PAYMENT_STATUS_SUCCESS;
            $dog = WATCHDOG_NOTICE;
            $msg = 'Payment was successful.';
            break;
          default: // shouldn't
            $status = COMMERCE_PAYMENT_STATUS_PENDING;
            $dog = WATCHDOG_WARNING;
            $msg = 'Payment is pending.';
            drupal_set_message(t(
              'The order is complete, but the payment is still pending by Alpha Bank. Please use the<a href=/contact">contact form</a> for further information.'));
            break;
        }
        break;
      case 'fail':
        $status = COMMERCE_PAYMENT_STATUS_FAILURE;
        $dog = WATCHDOG_ERROR;
        commerce_payment_redirect_pane_previous_page($order);
        switch ($data['status']) {
          case 'CANCELED':
            $msg = 'Payment was canceled.';
            break;
          case 'REFUSED':
            $msg = 'Payment was refused by Alpha Bank.';
            break;
          case 'ERROR':
          default:
            $msg = 'Error processing payment by Alpha Bank.';
            break;
        }
        drupal_set_message(t($msg), 'warning');
        break;
      case 'error':
      default:
        $status = COMMERCE_PAYMENT_STATUS_FAILURE;
        $dog = WATCHDOG_ERROR;
        $msg = 'Error processing payment.';
        drupal_set_message(t(
          'Error processing payment. Please use the<a href=/contact">contact form</a> for further information.'));
        // no commerce_payment_redirect_pane_previous_page here, earlier
        break;
    }
    $transaction->status = $status;
    $transaction->message = t($msg);
    commerce_payment_transaction_save($transaction);
  }
  else {
    $dog = WATCHDOG_ERROR;
    $msg = 'Ignoring new payment notification with invalid order ID';
  }
  watchdog('commerce_emporiki',
    $msg . ' (result: [!result], keys: [!keys], data: [!data])',
    array(
      '!result' => $result,
      '!keys' => implode(', ', array_keys($data)),
      '!data' => implode(', ', $data),
    ), $dog);
}